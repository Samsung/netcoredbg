#!/bin/bash

: ${TIMEOUT:=150}

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
    "MITestTarget"
    "MITestExceptionBreakpoint"
    "MITestExitCode"
    "MITestEvalNotEnglish"
    "MITest中文目录"
    "MITestSrcBreakpointResolve"
    "MITestEnum"
    "MITestAsyncStepping"
    "MITestBreak"
    "MITestBreakpointToModule"
    "MITestNoJMCNoFilterStepping"
    "MITestNoJMCBreakpoint"
    "MITestNoJMCAsyncStepping"
    "VSCodeExampleTest"
    "VSCodeTestBreakpoint"
    "VSCodeTestFuncBreak"
    "VSCodeTestAttach"
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
)

# Skipped tests:
# VSCodeTest297killNCD --- is not automated enough. For manual run only.

TEST_NAMES="$@"

if [[ -z $NETCOREDBG ]]; then
    NETCOREDBG="../bin/netcoredbg"
fi

if [[ -z $TEST_NAMES ]]; then
    TEST_NAMES="${ALL_TEST_NAMES[@]}"
fi

dotnet build TestRunner || exit $?

test_pass=0
test_fail=0
test_list=""

DOC=<<EOD
  test_timeout run a command with timelimit and with housekeeping of all child processes
  Usage: test_timeout <timeout> <command>

  Handles:
  * ^C (SIGINT)
  * SIGTERM and some another cases to terminate script
  * timeout
  * command termination with error code
  * deep tree of command's processes
  * broken-in-midle tree of command's processes (orphan subchildren)
  * set -e agnostic
EOD
test_timeout()(
    set +o | grep errexit | grep -qw -- -o && saved_errexit="set -e"

    kill_hard(){
        kill -TERM $1
        sleep 0.5
        kill -KILL $1
    } 2>/dev/null

    set -m
    (
        {
            sleep $1
            echo "task killed by timeout" >&2
            get_pgid() { set -- $(cat /proc/self/stat); echo $5; }
            kill -ALRM -$(get_pgid) >/dev/null 2>&1
        } &
        shift
        $saved_errexit
        "$@"
    ) &
    pgid=$!
      trap "kill -INT -$pgid; exit 130" INT
      trap "kill_hard -$pgid" EXIT RETURN TERM
    wait %+
)

trap "jobs -p | xargs -r -n 1 kill --" EXIT

for TEST_NAME in $TEST_NAMES; do
    dotnet build $TEST_NAME || {
        echo "$TEST_NAME: build error." >&2
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed: build error\n"
        continue
    }

    SOURCE_FILES=$(find $TEST_NAME \! -path "$TEST_NAME/obj/*" -type f -name "*.cs" -printf '%p;')

    PROTO="mi"
    if  [[ $TEST_NAME == VSCode* ]] ;
    then
        PROTO="vscode"
    fi

    test_timeout $TIMEOUT dotnet run --project TestRunner -- \
        --local $NETCOREDBG \
        --proto $PROTO \
        --test $TEST_NAME \
        --sources "$SOURCE_FILES" \
        --assembly $TEST_NAME/bin/Debug/netcoreapp3.1/$TEST_NAME.dll \
        "${LOGOPTS[@]}"

    res=$?

    if [ "$res" -ne "0" ]; then
        test_fail=$(($test_fail + 1))
        test_list="$test_list$TEST_NAME ... failed res=$res\n"
    else
        test_pass=$(($test_pass + 1))
        test_list="$test_list$TEST_NAME ... passed\n"
    fi
done

echo ""
echo -e $test_list
echo "Total tests: $(($test_pass + $test_fail)). Passed: $test_pass. Failed: $test_fail."
