#!/bin/bash

# This script tests:
# - tpk applications installation with and without NI generation (test debugger's pdb search routine)
# - launch_app work with debugger (in this way MSVS plugin work with debugger)

print_help()
{
    echo "Usage: sdb_run_tizen_tests.sh [OPTION]..."
    echo "Run functional tests on Tizen target device."
    echo ""
    echo "  -s, --sdb         sdb binary, \"sdb\" by default"
    echo "  -p, --port        local tcp port, \"4712\" by default"
    echo "  -d, --dotnet      dotnet binary, \"dotnet\" by default"
    echo "      --repeat      repeat tests, \"1\" by default"
    echo "  -g, --gbsroot     path to GBS root folder, \"\$HOME/GBS-ROOT\" by default"
    echo "  -r, --rpm         path to netcordbg rmp file"
    echo "      --help        display this help and exit"
}

# DO NOT CHANGE
# we use first test control program for both tests, since we need NI generation in second test
# make sure, that you have all sources synchronized
ALL_TEST_NAMES=(
    "TestApp1"
    "TestApp2"
)

SDB=${SDB:-sdb}
PORT=${PORT:-4712}
DOTNET=${DOTNET:-dotnet}
REPEAT=${REPEAT:-1}
GBSROOT=${GBSROOT:-$HOME/GBS-ROOT}
# launch_app have hardcoded path
TOOLS_ABS_PATH=/home/owner/share/tmp/sdk_tools
SCRIPTDIR=$(dirname $(readlink -f $0))

for i in "$@"
do
case $i in
    -s=*|--sdb=*)
    SDB="${i#*=}"
    shift
    ;;
    -p=*|--port=*)
    PORT="${i#*=}"
    shift
    ;;
    -d=*|--dotnet=*)
    DOTNET="${i#*=}"
    shift
    ;;
    --repeat=*)
    REPEAT="${i#*=}"
    shift
    ;;
    -g=*|--gbsroot=*)
    GBSROOT="${i#*=}"
    shift
    ;;
    -r=*|--rpm=*)
    RPMFILE="${i#*=}"
    shift
    ;;
    -h|--help)
    print_help
    exit 0
    ;;
    *)
    echo "Error: unknown option detected"
    exit 1
    ;;
esac
done

if [[ -z $RPMFILE ]]; then
    # Detect target arch
    if   $SDB shell lscpu | grep -q armv7l;  then ARCH=armv7l;
    elif $SDB shell lscpu | grep -q aarch64; then
        # https://superuser.com/questions/791506/how-to-determine-if-a-linux-binary-file-is-32-bit-or-64-bit
        # The 5th byte of a Linux binary executable file is 1 for a 32 bit executable, 2 for a 64 bit executable.
        if [ $($SDB shell od -An -t x1 -j 4 -N 1 /bin/od | grep "02") ]; then ARCH=aarch64;
        else ARCH=armv7l; fi
    elif $SDB shell lscpu | grep -q i686;    then ARCH=i686;
    else echo "Unknown target architecture"; exit 1; fi

    # The following command assumes that GBS build was performed on a clean system (or in Docker),
    # which means only one such file exists.
    RPMFILE=$(find $GBSROOT/local/repos/ -type f -name netcoredbg-[0-9]\*$ARCH.rpm -print -quit)
fi

# Repackage RPM file as TGZ

if [ ! -f "$RPMFILE" ]; then echo "Debugger RPM not found"; exit 1; fi
PKGNAME=`rpm -q --qf "%{n}" -p $RPMFILE`
PKGVERSION=`rpm -q --qf "%{v}" -p $RPMFILE`
PKGARCH=`rpm -q --qf "%{arch}" -p $RPMFILE`
TARGZNAME=$PKGNAME-$PKGVERSION-$PKGARCH.tar.gz
if [ -d "$SCRIPTDIR/unpacked" ]; then rm -rf "$SCRIPTDIR/unpacked"; fi
mkdir "$SCRIPTDIR/unpacked" && cd "$SCRIPTDIR/unpacked"
rpm2cpio "$RPMFILE" | cpio -idmv
touch .$TOOLS_ABS_PATH/$PKGNAME/version-$PKGVERSION
tar cfz ../$TARGZNAME --owner=owner --group=users -C .$TOOLS_ABS_PATH .
cd ..

# Upload TGZ to target and unpack

REMOTETESTDIR=$TOOLS_ABS_PATH/netcoredbg-tests

$SDB shell rm -rf "$TOOLS_ABS_PATH/netcoredbg"
$SDB shell mkdir -p $TOOLS_ABS_PATH/on-demand
$SDB push $TARGZNAME $TOOLS_ABS_PATH/on-demand
$SDB shell "cd $TOOLS_ABS_PATH && tar xf $TOOLS_ABS_PATH/on-demand/$(basename $TARGZNAME)"
$SDB shell rm -rf "$REMOTETESTDIR"
$SDB shell mkdir $REMOTETESTDIR

NETCOREDBG=$TOOLS_ABS_PATH/netcoredbg/netcoredbg

# Prepare
$DOTNET build $SCRIPTDIR/TestRunner
$SDB forward --remove tcp:$PORT
$SDB forward tcp:$PORT tcp:4711

test_pass=0
test_fail=0
test_list=""

for i in $(eval echo {1..$REPEAT}); do
# Build, install and run tests
for TEST_NAME in ${ALL_TEST_NAMES[@]}; do
    HOSTTESTDIR=$SCRIPTDIR/$TEST_NAME
    $DOTNET build $HOSTTESTDIR
    $SDB install $HOSTTESTDIR/bin/Debug/netcoreapp2.1/org.tizen.example.$TEST_NAME-1.0.0.tpk
 
    $SDB shell launch_app org.tizen.example.$TEST_NAME  __AUL_SDK__ NETCOREDBG __DLP_DEBUG_ARG__ --server=4711,--

    # DO NOT CHANGE
    # we use first test control program for both tests, since we need NI generation in second test
    # make sure, that you have all sources synchronized
    dotnet run --project TestRunner -- \
        --tcp localhost $PORT \
        --test $TEST_NAME \
        --sources "TestApp1/Program.cs" 

    if [ "$?" -ne "0" ]; then
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed\n"
    else
        test_pass=$(($test_pass + 1))
        test_list="$test_list$TEST_NAME ... passed\n"
    fi

    $SDB shell pkgcmd -u -n org.tizen.example.$TEST_NAME
done
done # REPEAT

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."

exit $test_fail
