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
    echo "  -c, --coverage    create code coverage report, do not create by default"
    echo "      --help        display this help and exit"
}

generate_xml()
{
    local xml_path=$1
    xml_filename="${xml_path}/test-results.xml"

    echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" > $xml_filename
    echo "    <testsuites>" >> $xml_filename
    echo "        <testsuite name=\"Tests\" tests=\"$(($test_pass+$test_fail))\" failures=\"$test_fail\">"  >> $xml_filename
    for item in ${test_xml[*]}
    do
        echo "            <testcase name=\"${item}" >> $xml_filename
    done
    echo "        </testsuite>" >> $xml_filename
    echo "    </testsuites>" >> $xml_filename
}

ALL_TEST_NAMES=(
    "CLITestBreakpoint"
    "CLITestInteropBreakpoint"
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
    "MITestExtensionMethods"
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
    "MITestHotReloadPDB"
    "MITestHotReloadUpdate"
    "MITestGeneric"
    "MITestEvalArraysIndexers"
    "MITestBreakpointWithoutStop"
    "MITestBreakpointUpdate"
    "MITestUnhandledException"
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
    "VSCodeTestExceptionBreakpoint"
    "VSCodeTestNoJMCExceptionBreakpoint"
    "VSCodeTestSizeof"
    "VSCodeTestAsyncLambdaEvaluate"
    "VSCodeTestGeneric"
    "VSCodeTestEvalArraysIndexers"
    "VSCodeTestExtensionMethods"
    "VSCodeTestBreakpointWithoutStop"
    "VSCodeTestUnhandledException"
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
XML_ABS_PATH=${XML_ABS_PATH:-build}
COVERAGE_DATA_DIR=${COVERAGE_DATA_DIR:-/home/abuild}
OBJS_DIR=${OBJS_DIR:-$COVERAGE_DATA_DIR/rpmbuild/BUILD}
CODE_COVERAGE_REPORT=false

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
    -c|--coverage)
    CODE_COVERAGE_REPORT=true
    shift
    ;;
    *)
        TEST_NAMES="$TEST_NAMES *"
    ;;
esac
done

TEST_NAMES="$@"

if [[ -z $TEST_NAMES ]]; then
    TEST_NAMES="${ALL_TEST_NAMES[@]}"
    # delete all accumulated coverage data
    $SDB root on
    $SDB shell rm -rf $COVERAGE_DATA_DIR
    $SDB root off
    rm -rf build
fi

set -x
# Detect target arch
if   $SDB shell lscpu | grep -q armv7l;  then ARCH=armv7l;
elif $SDB shell lscpu | grep -q aarch64; then
    # https://superuser.com/questions/791506/how-to-determine-if-a-linux-binary-file-is-32-bit-or-64-bit
    # The 5th byte of a Linux binary executable file is 1 for a 32 bit executable, 2 for a 64 bit executable.
    if [ $($SDB shell od -An -t x1 -j 4 -N 1 /bin/od | grep "02") ]; then ARCH=aarch64;
    else ARCH=armv7l; fi
elif $SDB shell lscpu | grep -q i686;    then ARCH=i686;
elif $SDB shell lscpu | grep -q x86_64;  then ARCH=x86_64;
elif $SDB shell lscpu | grep -q riscv64;  then ARCH=riscv64;
else echo "Unknown target architecture"; exit 1;
fi

if [[ -z $RPMFILE ]]; then
    # The following command assumes that GBS build was performed on a clean system (or in Docker),
    # which means only one such file exists.
    RPMFILE=$(find $GBSROOT/local/repos/ -type f -name netcoredbg-[0-9]\*$ARCH.rpm -print -quit)
fi
# Find tests related RPMs near debugger RPM.
RPMFILE_TEST=$(find $(dirname "${RPMFILE}") -type f -name netcoredbg-test-[0-9]\*$ARCH.rpm -print -quit)
RPMFILE_TESTDEBUG=$(find $(dirname "${RPMFILE}") -type f -name netcoredbg-test-debuginfo-[0-9]\*$ARCH.rpm -print -quit)

REMOTETESTDIR=$TOOLS_ABS_PATH/netcoredbg-tests

# Repackage RPM file as TGZ

if [ ! -f "$RPMFILE" ]; then echo "Debugger RPM not found"; exit 1; fi
PKGNAME=`rpm -q --qf "%{n}" -p $RPMFILE`
PKGVERSION=`rpm -q --qf "%{v}" -p $RPMFILE`
PKGARCH=`rpm -q --qf "%{arch}" -p $RPMFILE`
TARGZNAME=$PKGNAME-$PKGVERSION-$PKGARCH.tar.gz
if [ -d "$SCRIPTDIR/unpacked" ]; then rm -rf "$SCRIPTDIR/unpacked"; fi
mkdir "$SCRIPTDIR/unpacked" && cd "$SCRIPTDIR/unpacked"
rpm2cpio "$RPMFILE" | cpio -idmv
if [[ -f "$RPMFILE_TEST" ]] ;
then
    rpm2cpio "$RPMFILE_TEST" | cpio -idmv
else
    echo "Debugger Test RPM not found"
fi
if [[ -f "$RPMFILE_TESTDEBUG" ]] ;
then
    rpm2cpio "$RPMFILE_TESTDEBUG" | cpio -idmv
    # Note, "home" folder is a symlink with real location in Tizen is "/opt/usr/home", debug info located in "/usr/lib/debug/home/.." will be never found.
    # So, we just copy debuginfo close to library .so file.
    cp -a "./usr/lib/debug/home/owner/share/tmp/sdk_tools/netcoredbg-tests/." "./$REMOTETESTDIR"
    rm -rf "./usr/lib/debug/home/owner/share/tmp/sdk_tools/netcoredbg-tests/"
else
    echo "Debugger Test Debug Info RPM not found"
fi
touch .$TOOLS_ABS_PATH/$PKGNAME/version-$PKGVERSION
tar cfz ../$TARGZNAME --owner=owner --group=users -C .$TOOLS_ABS_PATH .
cd ..

# Upload TGZ to target and unpack

$SDB shell rm -rf "$TOOLS_ABS_PATH/netcoredbg"
$SDB shell rm -rf "$TOOLS_ABS_PATH/netcoredbg-tests"
$SDB shell mkdir -p $TOOLS_ABS_PATH/on-demand
$SDB push $TARGZNAME $TOOLS_ABS_PATH/on-demand
$SDB shell rm -rf "$REMOTETESTDIR"
$SDB shell mkdir $REMOTETESTDIR
$SDB shell "cd $TOOLS_ABS_PATH && tar xf $TOOLS_ABS_PATH/on-demand/$(basename $TARGZNAME)"

NETCOREDBG=$TOOLS_ABS_PATH/netcoredbg/netcoredbg

# Prepare
$DOTNET build $SCRIPTDIR/TestRunner
$SDB forward --remove tcp:$PORT
$SDB forward tcp:$PORT tcp:4711

test_pass=0
test_fail=0
test_count=0
test_list=""
test_xml=()

for i in $(eval echo {1..$REPEAT}); do
# Build, push and run tests
for TEST_NAME in $TEST_NAMES; do
    TEST_PROJ_NAME=$TEST_NAME;

    if  [[ $TEST_NAME == MITestHotReloadAsyncStepping ]] ;
    then
        TEST_PROJ_NAME="TestAppHotReloadAsync"
    elif  [[ $TEST_NAME == MITestHotReloadUpdate ]] ;
    then
        TEST_PROJ_NAME="TestAppHotReloadUpdate"
    elif  [[ $TEST_NAME == MITestHotReload* ]] ;
    then
        TEST_PROJ_NAME="TestAppHotReload"
    fi

    $DOTNET build $SCRIPTDIR/$TEST_PROJ_NAME
    $SDB push $SCRIPTDIR/$TEST_PROJ_NAME/bin/Debug/netcoreapp3.1/$TEST_PROJ_NAME.{dll,pdb} $REMOTETESTDIR

    HOSTTESTDIR=$SCRIPTDIR/$TEST_NAME
    SOURCE_FILES=$(find $HOSTTESTDIR \! -path "$HOSTTESTDIR/obj/*" -type f -name "*.cs" -printf '%p;')

    RC="0"

    if [[ $TEST_NAME == CLI* ]] ;
    then

        CLI_NETCOREDBG=$NETCOREDBG
        if  [[ $TEST_NAME == CLITestInterop* ]] ;
        then
            CLI_NETCOREDBG="$NETCOREDBG --interop-debugging"
        fi

        $SDB push $SCRIPTDIR/$TEST_PROJ_NAME/commands.txt $REMOTETESTDIR
        $SDB root on
        ./run_cli_test.sh "$SDB shell $CLI_NETCOREDBG" "$TEST_NAME" "$REMOTETESTDIR/$TEST_NAME.dll" "$REMOTETESTDIR/commands.txt"
        let RC=$?
        $SDB root off

    elif  [[ $TEST_NAME == VSCode* ]] ;
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
            --sdb "$SDB" \
            --test $TEST_NAME \
            --sources $SOURCE_FILES \
            --assembly $REMOTETESTDIR/$TEST_PROJ_NAME.dll
        let RC=$?
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
            --sdb "$SDB" \
            --test $TEST_NAME \
            --sources "$SOURCE_FILES" \
            --assembly $REMOTETESTDIR/$TEST_PROJ_NAME.dll
        let RC=$?
    fi

    if [ "$RC" -ne "0" ]; then
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed res=$res\n"
        test_xml[test_count]="$TEST_NAME\"><failure></failure></testcase>"
    else
        test_pass=$(($test_pass + 1))
        test_list="$test_list$TEST_NAME ... passed\n"
        test_xml[test_count]="$TEST_NAME\"></testcase>"
    fi
    test_count=$(($test_count + 1))
done
done # REPEAT

#Collect code coverage artifacts if enabled
if [[ $CODE_COVERAGE_REPORT == true ]]; then
    cp -r $GBSROOT/local/BUILD-ROOTS/scratch.$ARCH.0/$OBJS_DIR/$PKGNAME-$PKGVERSION/build .
    $SDB shell tar -czf $REMOTETESTDIR/coverage.tar.gz -C $OBJS_DIR/$PKGNAME-$PKGVERSION build
    $SDB pull $REMOTETESTDIR/coverage.tar.gz
    tar -xf coverage.tar.gz
    echo "geninfo_adjust_src_path = $COVERAGE_DATA_DIR => $GBSROOT/local/BUILD-ROOTS/scratch.$ARCH.0$COVERAGE_DATA_DIR" > build/lcov.cfg
    lcov --capture --derive-func-data --gcov-tool $PWD/llvm-gcov.sh --config-file build/lcov.cfg --directory build/src/CMakeFiles/netcoredbg.dir/ --output-file build/coverage.info
    lcov --remove build/coverage.info '*third_party/*' '/lib/*' '/lib64/*' '/usr/*' '*errormessage*' -o build/coverage.info
    genhtml -s -o cov_html build/coverage.info
    zip -r cov_html.zip cov_html
    rm coverage.tar.gz
fi

#Generate xml test file to XML_ABS_PATH
generate_xml "${XML_ABS_PATH}"

zip -D test-results.zip $XML_ABS_PATH/test-results.xml

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."

exit $test_fail
