﻿param(
    [String]$x,
    [Parameter(Mandatory = $false, Position = 0, ValueFromRemainingArguments = $true)]
    [String[]] $tests
)
# Making Windows PowerShell console window Unicode (UTF-8) aware.
$OutputEncoding = [console]::InputEncoding = [console]::OutputEncoding = New-Object System.Text.UTF8Encoding

$ALL_TEST_NAMES = @(
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
    "MITestExtensionMethods"
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
    "MITestNoJMCExceptionBreakpoint"
    "MITestSizeof"
    "MITestAsyncLambdaEvaluate"
    "MITestGeneric"
    "MITestEvalArraysIndexers"
    "MITestBreakpointWithoutStop"
    "MITestBreakpointUpdate"
    "MITestUnhandledException"
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
# VSCodeTest297killNCD --- is not automated enough. For manual run only.

$TEST_NAMES = $tests

if ($NETCOREDBG.count -eq 0) {
    $NETCOREDBG = "../bin/netcoredbg.exe"
}

if ($TEST_NAMES.count -eq 0) {
    $TEST_NAMES = $ALL_TEST_NAMES
}

# Prepare
dotnet build TestRunner
if ($lastexitcode -ne 0) {
    throw ("Exec build test: " + $errorMessage)
}

$test_pass = 0
$test_fail = 0
$test_list = ""
$test_xml = ""

# Build, push and run tests
foreach ($TEST_NAME in $TEST_NAMES) {
    dotnet build $TEST_NAME
    if ($lastexitcode -ne 0) {
        $test_fail++
        $test_list = "$test_list$TEST_NAME ... failed: build error`n"
        continue
    }

    $SOURCE_FILE_LIST = (Get-ChildItem -Path "$TEST_NAME" -Recurse -Filter *.cs | Where {$_.FullName -notlike "*\obj\*"} | Resolve-path -relative).Substring(2)

    $SOURCE_FILES = ""
    foreach ($SOURCE_FILE in $SOURCE_FILE_LIST) {
        $SOURCE_FILES += $SOURCE_FILE + ";"
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
        --assembly $TEST_NAME/bin/Debug/netcoreapp3.1/$TEST_NAME.dll


    if($?)
    {
        $test_pass++
        $test_list = "$test_list$TEST_NAME ... passed`n"
        $test_xml += "<testcase name=`"$TEST_NAME`"></testcase>"
    }
    else
    {
        $test_fail++
        $test_list = "$test_list$TEST_NAME ... failed`n"
        $test_xml += "<testcase name=`"$TEST_NAME`"><failure></failure></testcase>"
    }
}

#Generate xml test file to current directory
if($x -NotLike '')
{
    "<?xml version=`"1.0`" encoding=`"utf-16`" ?>
        <testsuites>
            <testsuite name=""Tests"" tests=`""" failures=`""" errors=`""" time=`""">
                $test_xml
            </testsuite>
        </testsuites>" | Out-File -FilePath "${x}\test-results.xml"
}

Write-Host ""
Write-Host $test_list
Write-Host "Total tests: $($test_pass + $test_fail). Passed: $test_pass. Failed: $test_fail."
