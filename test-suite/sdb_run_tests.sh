#!/bin/bash

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

TEST_NAMES="$@"

if [[ -z $TEST_NAMES ]]; then
    TEST_NAMES="${ALL_TEST_NAMES[@]}"
fi


SDB=${SDB:-sdb}

GBSROOT=$HOME/GBS-ROOT
TOOLS_ABS_PATH=/home/owner/share/tmp/sdk_tools

SCRIPTDIR=$(dirname $(readlink -f $0))

# Detect target arch

if   $SDB shell lscpu | grep -q armv7l; then ARCH=armv7l; 
elif $SDB shell lscpu | grep -q i686;   then ARCH=i686;
else echo "Unknown target architecture"; exit 1; fi

# The following command assumes that GBS build was performed on a clean system (or in Docker),
# which means only one such file exists.
RPMFILE=$(find $GBSROOT/local/repos/ -type f -name netcoredbg-[0-9]\*$ARCH.rpm -print -quit)

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
tar cfz ../$TARGZNAME --owner=root --group=root -C .$TOOLS_ABS_PATH .
cd ..

# Upload TGZ to target and unpack

REMOTETESTDIR=$TOOLS_ABS_PATH/netcoredbg-tests

$SDB root on
$SDB shell rm -rf "$TOOLS_ABS_PATH/netcoredbg"
$SDB shell mkdir -p $TOOLS_ABS_PATH/on-demand
$SDB push $TARGZNAME $TOOLS_ABS_PATH/on-demand
$SDB shell "cd $TOOLS_ABS_PATH && tar xf $TOOLS_ABS_PATH/on-demand/$(basename $TARGZNAME)"
$SDB shell rm -rf "$REMOTETESTDIR"
$SDB shell mkdir $REMOTETESTDIR

NETCOREDBG=$TOOLS_ABS_PATH/netcoredbg/netcoredbg

# Prepare
dotnet build $SCRIPTDIR/TestRunner
sdb forward tcp:4712 tcp:4711

test_pass=0
test_fail=0
test_list=""

# Build, push and run tests
for TEST_NAME in $TEST_NAMES; do
    HOSTTESTDIR=$SCRIPTDIR/$TEST_NAME
    dotnet build $HOSTTESTDIR
    sdb push $HOSTTESTDIR/bin/Debug/netcoreapp2.1/$TEST_NAME.{dll,pdb} $REMOTETESTDIR

    SOURCE_FILES=$(find $HOSTTESTDIR \! -path "$HOSTTESTDIR/obj/*" -type f -name "*.cs" -printf '%p;')

    if  [[ $TEST_NAME == VSCode* ]] ;
    then
        PROTO="vscode"

        # change $HOME to $REMOTETESTDIR in order to prevent /root/nohup.out creation
        sdb shell HOME=$REMOTETESTDIR nohup $NETCOREDBG --server --interpreter=$PROTO -- \
            /usr/bin/dotnet-launcher $REMOTETESTDIR/$TEST_NAME.dll

        dotnet run --project TestRunner -- \
            --tcp localhost 4712 \
            --proto $PROTO \
            --test $TEST_NAME \
            --sources $SOURCE_FILES
    else
        PROTO="mi"

        # change $HOME to /tmp in order to prevent /root/nohup.out creation
        sdb shell HOME=$REMOTETESTDIR nohup $NETCOREDBG --server --interpreter=$PROTO

        dotnet run --project TestRunner -- \
            --tcp localhost 4712 \
            --dotnet /usr/bin/dotnet-launcher \
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

# Leave
sdb root off

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."
