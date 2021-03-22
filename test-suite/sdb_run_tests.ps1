# Making Windows PowerShell console window Unicode (UTF-8) aware.
$OutputEncoding = [console]::InputEncoding = [console]::OutputEncoding = New-Object System.Text.UTF8Encoding

# Please prepare sdb target with netcoredbg before start
$NETCOREDBG = "/home/owner/share/tmp/sdk_tools/netcoredbg/netcoredbg"

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
    "MITestExceptionBreakpoint"
    "MITestExitCode"
    "MITestEvalNotEnglish"
    "MITestEnum"
    "MITestAsyncStepping"
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
    "VSCodeTestEnum"
    "VSCodeTestAsyncStepping"
)

# Skipped tests:
# MITest中文目录 and VSCodeTest中文目录 - sdb related issue with non-English assembly/pdb name during 'push'
# VSCodeTestSrcBreakpointResolve and MITestSrcBreakpointResolve - case sensitive test for paths work different on Linux/Windows parts

$TEST_NAMES = $args

if ($TEST_NAMES.count -eq 0) {
    $TEST_NAMES = $ALL_TEST_NAMES
}

# Prepare
dotnet build TestRunner

sdb root on
sdb forward tcp:4712 tcp:4711

$test_pass = 0
$test_fail = 0
$test_list = ""

# Build, push and run tests
foreach ($TEST_NAME in $TEST_NAMES) {
    dotnet build $TEST_NAME

    sdb push $TEST_NAME\bin\Debug\netcoreapp3.1\$TEST_NAME.dll /tmp/
    sdb push $TEST_NAME\bin\Debug\netcoreapp3.1\$TEST_NAME.pdb /tmp/
    sdb shell chsmack -a User::App::Shared /tmp/$TEST_NAME.dll
    sdb shell chsmack -a User::App::Shared /tmp/$TEST_NAME.pdb

    $SOURCE_FILE_LIST = (Get-ChildItem -Path "$TEST_NAME" -Recurse -Filter *.cs | Where {$_.FullName -notlike "*\obj\*"} | Resolve-path -relative).Substring(2)

    $SOURCE_FILES = ""
    foreach ($SOURCE_FILE in $SOURCE_FILE_LIST) {
        $SOURCE_FILES += $SOURCE_FILE + ";"
    }

    if ($TEST_NAME.StartsWith("VSCode")) {
        $PROTO = "vscode"

        # change $HOME to /tmp in order to prevent /root/nohup.out creation
        sdb shell HOME=/tmp nohup $NETCOREDBG --server --interpreter=$PROTO -- `
            /usr/bin/dotnet /tmp/$TEST_NAME.dll

        dotnet run --project TestRunner -- `
            --tcp localhost 4712 `
            --proto $PROTO `
            --test $TEST_NAME `
            --sources $SOURCE_FILES `
            --assembly /tmp/$TEST_NAME.dll
    } else {
        $PROTO = "mi"

        # change $HOME to /tmp in order to prevent /root/nohup.out creation
        sdb shell HOME=/tmp nohup $NETCOREDBG --server --interpreter=$PROTO

        dotnet run --project TestRunner -- `
            --tcp localhost 4712 `
            --dotnet /usr/bin/dotnet `
            --proto $PROTO `
            --test $TEST_NAME `
            --sources $SOURCE_FILES `
            --assembly /tmp/$TEST_NAME.dll
    }


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

# Leave
sdb root off

Write-Host ""
Write-Host $test_list
Write-Host "Total tests: $($test_pass + $test_fail). Passed: $test_pass. Failed: $test_fail."
