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
    echo "                    \"/tmp\" by default"
    echo "  -r, --rpm         path to netcordbg rmp file"
    echo "  -x, --xml_path    path to test-results xml xunit format file,"
    echo "                    \"/home/owner/share/tmp/\" by default"
    echo "      --help        display this help and exit"
}
generate_xml()
{
    local xml_path=$1
    local testnames=$2

    echo "<?xml version=\"1.0\" encoding=\"utf-8\" ?>
        <testsuites>
            <testsuite name=\"Tests\" tests=\"\" failures=\"\" errors=\"\" time=\"\">
                ${testnames}
            </testsuite>
        </testsuites>" > "${xml_path}/test-results.xml"
}

ALL_TEST_NAMES=(
    "MIExampleTest"
    "MITestBreakpoint"
    "MITestExpression"
    "MITestVariables"
    "MITestStepping"
    "MITestEvaluate"
    "MITestException"
    "MITestEnv"
    "MITestGDB"
    "MITestExecAbort"
    "MITestExecInt"
    "MITestHandshake"
    "MITestExceptionBreakpoint"
    "MITestExitCode"
    "MITestEvalNotEnglish"
    "MITest中文目录"
    "MITestSrcBreakpointResolve"
    "MITestEnum"
    "MITestAsyncStepping"
    "MITestBreak"
    "MITestNoJMCNoFilterStepping"
    "MITestNoJMCBreakpoint"
    "MITestNoJMCAsyncStepping"
    "MITestNoJMCExceptionBreakpoint"
    "MITestSizeof"
    "MITestAsyncLambdaEvaluate"
    "MITestHotReloadAsyncStepping"
    "MITestHotReloadBreak"
    "MITestHotReloadBreakpoint"
    "MITestHotReloadEvaluate"
    "MITestHotReloadStepping"
    "MITestHotReloadJMC"
    "MITestHotReloadWithoutBreak"
    "MITestGeneric"
    "VSCodeExampleTest"
    "VSCodeTestBreakpoint"
    "VSCodeTestFuncBreak"
    "VSCodeTestPause"
    "VSCodeTestDisconnect"
    "VSCodeTestThreads"
    "VSCodeTestVariables"
    "VSCodeTestEvaluate"
    "VSCodeTestStepping"
    "VSCodeTestEnv"
    "VSCodeTestExitCode"
    "VSCodeTestEvalNotEnglish"
    "VSCodeTest中文目录"
    "VSCodeTestSrcBreakpointResolve"
    "VSCodeTestEnum"
    "VSCodeTestAsyncStepping"
    "VSCodeTestBreak"
    "VSCodeTestNoJMCNoFilterStepping"
    "VSCodeTestNoJMCBreakpoint"
    "VSCodeTestNoJMCAsyncStepping"
    "VSCodeTestSizeof"
    "VSCodeTestAsyncLambdaEvaluate"
    "VSCodeTestGeneric"
)

# Skipped tests:
# MITestBreakpointToModule - script don't support multiple DLLs copy to target

SDB=${SDB:-sdb}
PORT=${PORT:-4712}
DOTNET=${DOTNET:-dotnet}
REPEAT=${REPEAT:-1}
GBSROOT=${GBSROOT:-$HOME/GBS-ROOT}
TOOLS_ABS_PATH=${TOOLS_ABS_PATH:-/home/owner/share/tmp/sdk_tools}
SCRIPTDIR=$(dirname $(readlink -f $0))
XML_ABS_PATH=${XML_ABS_PATH:-/tmp}

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
    -x=*|--xml_path=*)
    XML_ABS_PATH="${i#*=}"
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
test_xml=""

for i in $(eval echo {1..$REPEAT}); do
# Build, push and run tests
for TEST_NAME in $TEST_NAMES; do
    TEST_PROJ_NAME=$TEST_NAME;

    if  [[ $TEST_NAME == MITestHotReloadAsyncStepping ]] ;
    then
        TEST_PROJ_NAME="TestAppHotReloadAsync"
    elif  [[ $TEST_NAME == MITestHotReload* ]] ;
    then
        TEST_PROJ_NAME="TestAppHotReload"
    fi

    $DOTNET build $SCRIPTDIR/$TEST_PROJ_NAME
    $SDB push $SCRIPTDIR/$TEST_PROJ_NAME/bin/Debug/netcoreapp3.1/$TEST_PROJ_NAME.{dll,pdb} $REMOTETESTDIR

    HOSTTESTDIR=$SCRIPTDIR/$TEST_NAME
    SOURCE_FILES=$(find $HOSTTESTDIR \! -path "$HOSTTESTDIR/obj/*" -type f -name "*.cs" -printf '%p;')

    if  [[ $TEST_NAME == VSCode* ]] ;
    then
        PROTO="vscode"

        # change $HOME to $REMOTETESTDIR in order to prevent /root/nohup.out creation
        $SDB root on
        $SDB shell HOME=$REMOTETESTDIR nohup $NETCOREDBG --server --interpreter=$PROTO -- \
             /usr/bin/dotnet $REMOTETESTDIR/$TEST_PROJ_NAME.dll
        $SDB root off

        $DOTNET run --project TestRunner -- \
            --tcp localhost $PORT \
            --proto $PROTO \
            --test $TEST_NAME \
            --sources $SOURCE_FILES \
            --assembly $REMOTETESTDIR/$TEST_PROJ_NAME.dll
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
            --assembly $REMOTETESTDIR/$TEST_PROJ_NAME.dll
    fi

    if [ "$?" -ne "0" ]; then
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed\n"
        test_xml+="<testcase name=\"$TEST_NAME\"><failure></failure></testcase>"
    else
        test_pass=$(($test_pass + 1))
        test_list="$test_list$TEST_NAME ... passed\n"
        test_xml+="<testcase name=\"$TEST_NAME\"></testcase>"
    fi
done
done # REPEAT

#Generate xml test file to XML_ABS_PATH
generate_xml "${XML_ABS_PATH}" "${test_xml}"

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."

exit $test_fail
