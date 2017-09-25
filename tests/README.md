# netcoregbd tests

## Running tests

```
git clone https://github.sec.samsung.net/i-kulaychuk/netcoredbg.git
cd netcoredbg
dotnet test
```

1. To run tests with dotnet:

```
CORERUN="/path/to/corerun" DEBUGGER="/path/to/debugger" dotnet test
```

2. To run tests with corerun:
After ```dotnet build``` nuget packages will be downloaded into ~/.nuget
To run tests with corerun, probably, you should copy some libraries from nuget packages into folder with corerun, like Microsoft.CodeAnalysis.*.dll and xunit.assert.dll (corerun will say if it miss some library).

```
dotnet build
CORERUN="/path/to/corerun" DEBUGGER="/path/to/debugger" /path/to/corerun /path/to/launcher.dll
```

Result logs will appear in netcoredbg/tests/runner/bin/Debug/netcoreapp2.0/<test_start_date>

## Creating test steps

- Сreate some folder for test case inside 'tests' dir.
- Сreate 'test_case_name.csproj' file and run ```dotnet sln add /path/to/test_case_name.csproj```.
- Сreate 'test_case_name.cs' file with test case as written below.

## Writing test case

1. See example test case inside 'tests/example' folder.
1. Write 'test_case_name.cs' file, which will be launched under the debugger.
1. Write scenario for debugger in comments inside 'test_case_name.cs' file.

### Writing scenario

You should write scenario for debugger inside comments in 'test_case_name.cs' file, this comments will be executed with Roslyn.

#### Multiline comments

Don't use ```/**/``` for multiline comment. If you want to write comment on line with Roslyn commands - you should write it, using '/**/'.
For multiline comments ```_currentLine``` and ```_commandLine``` variables will always be equal to number of first line.

#### You must do

- Write ```// $Main$``` tag on line with curly bracket after 'Main()'. At the beginning of every test case debugger running to Main.
- Write ```// start()``` on line, where you want to start test case. Debugger will run to line with this comment.
- Write ```// send("-gdb-exit")``` to finish test case.

#### Global test case funciotns

- ```MICore.ResultValue send(string cmd, int expectedLine = -1, int times = 1)``` - to send command 'cmd' to debugger. If command is '-exec-run', '-exec-continue', '-exec-step' or '-exec-next' - function will wait for the debugger to stop on 'expectedLine'. This function returns parsed respond: "\*stopped.*" for move commands and "\^done.*" for rest commands.
- ```Match expect(string s)``` - to find line 's' in debugger output, using regexp and return 'RegularExpressions.Match'. If this function wont find match in 'EXPECT_TIMEOUT' seconds - it will throw exception.
- ```void assertEqualRe(string expect, string actual, string msg = "", bool expectedRes = true)``` - to compare 'expect' and 'actual' strings with regexp and fail test, in case the result not equal to 'expectedRes'

#### Global test case variables

- ```int _current_line``` - has value of current line of '.cs' file. Functions 'nextTo()', 'stepTo()' and 'breakTo()' changing value of this variable, so if you use several of this functions inside single comment - '_current_line' variable will change while the comment is being executed.
- ```int _command_line``` - has value of current line of '.cs' file. Unlike '_current_line', '_command_line' will have same value during all comment.
- ```Dictionary<string, int> Tags``` - Dictionary, which contains line numbers with tags.
  - write ```// $ tag_name $``` to add line number to 'Tags' dictionary with 'tag_name' key.

#### Example

```
int x = 13;            // send("-exec-step", expectedLine: Tags["MY_TAG1"]);
Console.WriteLine(x);  // /* $ MY_TAG1 $ */
                       // var r = send(@"-var-create - * ""x""");
                       // assertEqualRe("13", r.Find("value").ToString());
```
