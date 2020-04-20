#!/bin/bash

print_help()
{
    echo "Usage: sdb_run_tests.sh [OPTION]... [TEST NAME]..."
    echo "Run functional tests on Tizen target device."
    echo ""
    echo "  -s, --sdb         sdb binary, \"sdb\" by default"
    echo "  -p, --port        local tcp port, \"4712\" by default"
    echo "  -d, --dotnet      dotnet binary, \"dotnet\" by default"
    echo "      --repeat      repeat tests, \"1\" by default"
    echo "  -g, --gbsroot     path to GBS root folder, \"\$HOME/GBS-ROOT\" by default"
    echo "  -p, --tools_path  path to tools dir on target,"
    echo "                    \"/home/owner/share/tmp/sdk_tools\" by default"
    echo "  -r, --rpm         path to netcordbg rmp file"
    echo "      --help        display this help and exit"
}

ALL_TEST_NAMES=(
    "MIExampleTest"
    "MITestBreakpoint"
    "MITestExpression"
    "MITestSetValue"
    "MITestStepping"
    "MITestVarObject"
    "MITestException"
    "MITestLambda"
    "MITestEnv"
    "MITestGDB"
    "MITestExecFinish"
    "MITestExecAbort"
    "MITestExecInt"
    "MITestHandshake"
    "MITestExceptionBreakpoint"
    "VSCodeExampleTest"
    "VSCodeTestBreakpoint"
    "VSCodeTestFuncBreak"
    "VSCodeTestPause"
    "VSCodeTestDisconnect"
    "VSCodeTestThreads"
    "VSCodeTestVariables"
    "VSCodeTestEvaluate"
    "VSCodeTestStepping"
)

SDB=${SDB:-sdb}
PORT=${PORT:-4712}
DOTNET=${DOTNET:-dotnet}
REPEAT=${REPEAT:-1}
GBSROOT=${GBSROOT:-$HOME/GBS-ROOT}
TOOLS_ABS_PATH=${TOOLS_ABS_PATH:-/home/owner/share/tmp/sdk_tools}
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
    -p=*|--tools_path=*)
    TOOLS_ABS_PATH="${i#*=}"
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
        TEST_NAMES="$TEST_NAMES *"
    ;;
esac
done

TEST_NAMES="$@"

if [[ -z $TEST_NAMES ]]; then
    TEST_NAMES="${ALL_TEST_NAMES[@]}"
fi

if [[ -z $RPMFILE ]]; then
    # Detect target arch
    if   $SDB shell lscpu | grep -q armv7l;  then ARCH=armv7l;
    elif $SDB shell lscpu | grep -q aarch64; then ARCH=armv7l;
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
# Build, push and run tests
for TEST_NAME in $TEST_NAMES; do
    HOSTTESTDIR=$SCRIPTDIR/$TEST_NAME
    $DOTNET build $HOSTTESTDIR
    $SDB push $HOSTTESTDIR/bin/Debug/netcoreapp2.1/$TEST_NAME.{dll,pdb} $REMOTETESTDIR

    SOURCE_FILES=$(find $HOSTTESTDIR \! -path "$HOSTTESTDIR/obj/*" -type f -name "*.cs" -printf '%p;')

    if  [[ $TEST_NAME == VSCode* ]] ;
    then
        PROTO="vscode"

        # change $HOME to $REMOTETESTDIR in order to prevent /root/nohup.out creation
        $SDB root on
        $SDB shell HOME=$REMOTETESTDIR nohup $NETCOREDBG --server --interpreter=$PROTO -- \
             /usr/bin/dotnet $REMOTETESTDIR/$TEST_NAME.dll
        $SDB root off

        $DOTNET run --project TestRunner -- \
            --tcp localhost $PORT \
            --proto $PROTO \
            --test $TEST_NAME \
            --sources $SOURCE_FILES
    else
        PROTO="mi"

        # change $HOME to /tmp in order to prevent /root/nohup.out creation
        $SDB root on
        $SDB shell HOME=$REMOTETESTDIR nohup $NETCOREDBG --server --interpreter=$PROTO
        $SDB root off

        $DOTNET run --project TestRunner -- \
            --tcp localhost $PORT \
            --dotnet /usr/bin/dotnet \
            --proto $PROTO \
            --test $TEST_NAME \
            --sources "$SOURCE_FILES" \
            --assembly $REMOTETESTDIR/$TEST_NAME.dll
    fi

    if [ "$?" -ne "0" ]; then
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed\n"
    else
        test_pass=$(($test_pass + 1))
        test_list="$test_list$TEST_NAME ... passed\n"
    fi
done
done # REPEAT

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."

exit $test_fail
