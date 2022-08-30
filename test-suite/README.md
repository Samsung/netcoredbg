# Prerequisites

Netcoredbg test suite requires **.NET SDK version 3.1**, you can download it manually from here: https://dotnet.microsoft.com/en-us/download/dotnet/3.1

# How to launch MIExampleTest for tcp netcoredbg server

- launch target and install netcoredbg;

- launch netcoredbg as tcp server on target via sdb shell:
```
    $ sdb shell nohup <path-to-netcoredbg> --server
```

- forward tcp port from target to host:
```
    $ sdb forward tcp:4712 tcp:4711
```

- move to test-suite directory;

- build MIExampleTest target assembly and TestRunner:
```
    $ dotnet build MIExampleTest
    $ dotnet build TestRunner
```
or you can build solution which consists of TestRunner and all tests:
```
    $ dotnet build
```

- push dll and pdb files into target /tmp/ disrectory:
```
    $ sdb push MIExampleTest/bin/Debug/netcoreapp3.1/MIExampleTest{pdb,dll} /tmp/
```

- change smack permissions:
```
    $ sdb shell chsmack -a "_" /tmp/MIExampleTest.{pdb,dll}
```

- launch TestRunner with tcp client (repsonses will be parsed only as GDB/MI outputs):

```
    $ dotnet run --project TestRunner -- \
        --tcp localhost 4712 \
        --dotnet /usr/share/dotnet/corerun \
        --test MIExampleTest \
        --sources MIExampleTest/Program.cs \
        --assembly /tmp/MIExampleTest.dll
```

- expect successfull passing of test.

Also after preparing sdb target with netcoredbg you can run follow script
to make all necessary steps.

- On Linux:
```
    launch all tests:
    $ ./sdb_run_tests.sh
    or
    $ ./sdb_run_tests.sh <test-name> [<test-name>]
```
- On Windows:
```
    launch all tests:
    > powershell.exe -executionpolicy bypass -File sdb_run_tests.ps1
    or
    > powershell.exe -executionpolicy bypass -File sdb_run_tests.ps1 <test-name> [<test-name>]
```

# How to launch tests locally

- On Linux:
```
    launch all tests:
    $ ./run_tests.sh
    or
    $ ./run_tests.sh <test-name> [<test-name>]
    or
    $ NETCOREDBG=<path-to-netcoredbg> ./run_tests.sh <test-name> [<test-name>]
```

- On Windows:
```
    launch all tests:
    $ powershell.exe -executionpolicy bypass -File run_tests.ps1
    or
    $ powershell.exe -executionpolicy bypass -File run_tests.ps1 <test-name> [<test-name>]
```

# How to add new test

- move to test-suite directory;

- create new project in test-suite folder:
```
    $ dotnet new console -o NewTest --framework netcoreapp3.1
```

- add reference to NetcoreDbgTest library:
```
    $ dotnet add NewTest/NewTest.csproj reference NetcoreDbgTest/NetcoreDbgTest.csproj
```

- add project NewTest to solution:
```
    $ dotnet sln add NewTest/NewTest.csproj
```

- add test name into ALL_TEST_NAMES list in "run_tests.sh", "run_tests.ps1", "sdb_run_tests.sh" and "sdb_run_tests.ps1" scripts;

- in MIExampleTest implemented small scenario of NetCoreDbgTest library using.
