$ALL_TEST_NAMES = @(
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
    "MITestTarget"
    "MITestExceptionBreakpoint"
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
)

$TEST_NAMES = $args

if ($NETCOREDBG.count -eq 0) {
    $NETCOREDBG = "../bin/netcoredbg.exe"
}

if ($TEST_NAMES.count -eq 0) {
    $TEST_NAMES = $ALL_TEST_NAMES
}

# Prepare
dotnet build TestRunner

$test_pass = 0
$test_fail = 0
$test_list = ""

# Build, push and run tests
foreach ($TEST_NAME in $TEST_NAMES) {
    dotnet build $TEST_NAME

    $SOURCE_FILE_LIST = (Get-ChildItem -Path $TEST_NAME -Recurse *.cs |
        ? { $_.FullName -inotmatch "$TEST_NAME\\obj" }).Name

    $SOURCE_FILES = ""
    foreach ($SOURCE_FILE in $SOURCE_FILE_LIST) {
        $SOURCE_FILES += $TEST_NAME + "/" + $SOURCE_FILE + ";"
    }

    $PROTO = "mi"
    if ($TEST_NAME.StartsWith("VSCode")) {
        $PROTO = "vscode"
    }

    dotnet run --project TestRunner -- `
        --local $NETCOREDBG `
        --proto $PROTO `
        --test $TEST_NAME `
        --sources $SOURCE_FILES `
        --assembly $TEST_NAME/bin/Debug/netcoreapp2.1/$TEST_NAME.dll


    if($?)
    {
        $test_pass++
        $test_list = "$test_list$TEST_NAME ... passed`n"
    }
    else
    {
        $test_fail++
        $test_list = "$test_list$TEST_NAME ... failed`n"
    }
}

Write-Host ""
Write-Host $test_list
Write-Host "Total tests: $($test_pass + $test_fail). Passed: $test_pass. Failed: $test_fail."
