# Tests

## Running tests

By default test runner looks for debugger binary in `./bin` directory and expects `dotnet` CLI to be present in `PATH`.

Test are run through dotnet CLI from the project root directory:
```
cd tests
dotnet build && dotnet test runner
```

It is possible to override path to the debugger binary through `PIPE` environment variable:
```
cd tests
PIPE=<path-to-debugger> dotnet test --logger "trx;LogFileName=$PWD/Results.trx"
```

`PIPE` can contain multiple shell commands, but in the end the debugger MI console should be lanched.

Test results can be exported to a file with `--logger` command line option.

By default test binaries are discovered recursively under project `tests` directory. When tests should be executed on another machine, environment variable `TESTDIR` can override the location of test binaries. In that case recursive search is not performed and all test binaries should be located directly under `TESTDIR`. So for `TESTDIR=/tmp` the test binary files layout shoud be:
```
/tmp/Example1Test.dll
/tmp/Example1Test.pdb
/tmp/Example2Test.dll
/tmp/Example2Test.pdb
...
```

Refer to `run_tests_sdb.sh` as an example of running tests on Tizen device.

`TIMEOUT` environment variable overrides a default timeout (in seconds) for test's `Expect` command.

## Writing new tests

### Add new test

Create new project under `tests` directory and add it to `tests` solution:
```
cd tests
dotnet new console -o ExampleTest
dotnet sln add ExampleTest/ExampleTest.csproj
```

Add project name as a method of `Runner.TestRunner` class in `runner/Runner.cs`:
```
[Fact]
public void ExampleTest() => ExecuteTest();
```

Method name should match the project name and the name of the dll that is produced after project build.

Now the ExampleTest should be displayed as available:
```
$ dotnet test -t
...
The following Tests are available:
    ...
    Runner.TestRunner.ExampleTest
```

### Write test scenario

Test scenario should be present in comments inside TestExample/Program.cs

The following example instructs the debugger to run test binary and verifies that the debugger stops at line with `@START@` tag with stop reason `entry-point-hit`:
```
/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
Send("3-exec-run");

var r = Expect("*stopped");
Assert.Equal(r.FindString("reason"), "entry-point-hit");
Assert.Equal(r.Find("frame").FindInt("line"), Lines["START"]);
*/
using System;

namespace ExampleTest
{
    class Program
    {
        static void Main(string[] args)
        { // //@START@
            Console.WriteLine("Hello World!");
        }
    }
}
```

#### Variables

* `Dictionary<string, int> Lines` provides the mapping between tags `@TAGNAME@` in test source and line numbers.

* `string TestSource` is a full path to test source file.

* `string TestBin` is a full path to test dll file.

* `int DefaultExpectTimeout` is a timeout for `Expect` operation, default value is 20 (seconds).

* `ITestOutputHelper Output` is a log interface from Xunit, it contains methods like `WriteLine` etc.

#### Methods

* `Assert.*` are asserts from Xunit.

* `void Send(string s)` sends command to the debugger process through stdin. New line is automatically appended to `s`.

* `MICore.Results Expect(string s, int timeoutSec = DefaultExpectTimeout)` reads debugger output line by line and checks if some line starts with `s`. If the expected line is found then it is parsed into MI responce object `MICore.Results`. In case of timeout or invalid MI responce an exception is thrown.
For more info about MI responce object see https://github.com/Microsoft/MIEngine/blob/master/src/MICore/MIResults.cs

* `int GetCurrentLine()` provides current line number.
