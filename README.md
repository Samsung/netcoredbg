# Debugger for .NET Core runtime

The debugger provides [GDB/MI](https://sourceware.org/gdb/current/onlinedocs/gdb/GDB_002fMI.html)
and [VSCode Debug Adapterprotocol](https://microsoft.github.io/debug-adapter-protocol/)
and allows to debug .NET apps under .NET Core runtime.  Also debugger allows debugging from
command line (like as GDB).

## Copyright

You can find licensing information in file [LICENSE](LICENSE), in root directory of Netcoredbg sources.

## Usage
More details about usage of NCDB you can find in [CLI](doc/cli.md) manual.


## Building from source code

Currently Netcoredbg can be built on Linux, MacOS or Windows. Instructions for building Netcoredbg on each platform is shown below.


### Unix

Building of Netcoredbg requires Microsoft's .NET, so currently you can build Netcoredbg only in Linux. Microsoft supports at least few distributions, see details here: https://docs.microsoft.com/en-us/dotnet/core/install/linux

#### Prerequisites

1. You need to install `cmake`, and `make` or `ninja`.

2. You need clang C++ compiler installed (Netcoredbg can't be built with gcc).

3. Microsoft's **.NET runtime** should be installed, you can download it here: https://dotnet.microsoft.com/download

4. May be you need to install some typical developers tools not mentioned here, like `git`, etc...

5. It is expected, that Netcoredbg sources placed to some directory;

6. Optional step: Netcoredbg requires **Core CLR runtime source code**, which is typically downloaded automatically, but you can download it from here: https://github.com/dotnet/coreclr

   You should check out tag v3.x.

7. Optional step: Netcoredbg requires **.NET SDK**, which can be downloaded automatically, but you can download it manually from here: https://dotnet.microsoft.com/download
   You need .NET SDK 3.1.

#### Compiling

Configure build with the following commands:

```
user@netcoredbg$ mkdir build
user@netcoredbg$ cd build
user@build$ CC=clang CXX=clang++ cmake ..
```

For running tests after build has succeed you need to add option `-DCMAKE_INSTALL_PREFIX=$PWD/../bin`.

If you have previously downloaded .NET SDK or Core CLR sources, then you should modify command line and add following options: `-DDOTNET_DIR=/path/to/sdk/dir -DCORECLR_DIR=/path/to/coreclr/sources`

If cmake tries to download .NET SDK or Core CLR sources and fails -- see bullets 6 and 7 above. You can download required files manually.

After configuration has finished, you can build Netcoredbg:

```
user@netcoredbg$ make
...
user@netcoredbg$ make install
```

To perform build from scratch (including configuration step) again you should delete artefacts with following commands:

```
user@build$ cd ..
user@netcoredbg$ rm -rf build src/debug/netcoredbg/bin bin
```

> *Directory `bin` contains "installed" Netcoredbg's binaries for tests. If you have installed Netcoredbg in other place, for example in `/usr/local/bin`, you should remove it manually: currently Netcoredbg's build system doesn't performs "uninstalling".*


### MacOS

You need install homebrew from here: https://brew.sh/

After this, build instructions are same as for Unix (including prerequisites).


### Windows

#### Prerequisites:

1. Download and install **CMake** from here: https://cmake.org/download

2. Download and install **Microsoft's Visual Studio 2019** or newer: https://visualstudio.microsoft.com/downloads

   During installation of Visual Studio you should install all options required
   for C# and C++ development on windows.

3. Download and install **Git**, you have few options here:

 * use original Git: https://git-scm.com/download/win
 * use TortoiseGit: https://tortoisegit.org/download
 * or use git installed in cygwin: https://cygwin.com/install.html

4. Checkout Netcoredbg sources to some directory by using git.


5. This step might be omitted, in this case cmake automatically downloads necessary files.
   But if it fails, you should then checkout **Core CLR sources** to another directory from here: https://github.com/dotnet/coreclr

   You need latest tag **v3.x**.

6. This step might be omitted too, and cmake will automatically downloads that it needs.
   But in case of failure you need download and install **.NET Core 3.1 SDK** from here: https://dotnet.microsoft.com/download

#### Compiling

Configure the build with the following commands given in Netcoredbg's source tree:

```
C:\Users\localuser\netcoredbg> md build
C:\Users\localuser\netcoredbg> cd build
C:\Users\localuser\netcoredbg\build> cmake .. -G "Visual Studio 16 2019"
```

You should run this command from cmd.exe, *not from cygwin's shell*.

Option `-G` specifies which instance of Visual Studio should build the project.
Note, minimum requirements for netcoredbg build is `Visual Studio 2019` version.


If you want to run tests after build succeed, you should add following option: `-DCMAKE_INSTALL_PREFIX="%cd%\..\bin"`

If you have downloaded .NET SDK or .NET Core sources manually, you should add following options:
`-DDOTNET_DIR="c:\Program Files\dotnet" -DCORECLR_DIR="path\to\coreclr"`


To compile and install give command:

```
C:\Users\localuser\netcoredbg\build> cmake --build . --target install
```


To perform build from scratch (including configuration step) again you should delete artefacts with following commands:

```
C:\Users\localuser\netcoredbg\build>cd ..
C:\Users\localuser\netcoredbg>rmdir /s /q build src\debug\netcoredbg\bin bin
```

> *Directory `bin` contains "installed" Netcoredbg's binaries for tests. If you have installed Netcoredbg in other place, you should remove it manually: currently Netcoredbg's build system doesn't performs "uninstalling".*



## Running Netcoredbg

In instructions shown above `netcoredbg` binary and additional libraries will be installed in some directory.
For developing purposes (for running tests, debugging, etc...) directory `bin` in Netcoredbg's source tree is typically used.

Now running the debugger with `--help` option should look like this:

```
$ ../bin/netcoredbg --help
.NET Core debugger

Options:
--buildinfo                           Print build info.
--attach <process-id>                 Attach the debugger to the specified process id.
--interpreter=cli                     Runs the debugger with Command Line Interface.
--interpreter=mi                      Puts the debugger into MI mode.
--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.
--command=<file>                      Interpret commands file at the start.
-ex "<command>"                       Execute command at the start
--run                                 Run program without waiting commands
--engineLogging[=<path to log file>]  Enable logging to VsDbg-UI or file for the engine.
                                      Only supported by the VsCode interpreter.
--server[=port_num]                   Start the debugger listening for requests on the
                                      specified TCP/IP port instead of stdin/out. If port is not specified
                                      TCP 4711 will be used.
--log[=<type>]                        Enable logging. Supported logging to file and to dlog (only for Tizen)
                                      File log by default. File is created in 'current' folder.
--version                             Displays the current version.

```

Basically, to debug .NET code you should run Netcoredbg with the following command line:
```
$ /path/to/netcoredbg --interpreter=TYPE -- /path/to/corerun /path/to/program.dll
```

## Notes for developers

### Running the tests

You can find detailed instruction how to run tests in `test-suite` directory, see [test-suite/README.md](test-suite/REDME.md).
Basically you just need to build and install Netcoredbg into `bin` directory (in Netcoredbg source tree) and then change directory to `test-suite` and run script `/run_tests.sh`


### Building and running unit tests

To build unit tests you need to add following option to CMake: `-DBUILD_TESTING=ON`.

After the build, you can run unit tests by the command: `make test`.

See details in [src/unittests/README.md](src/unittests/README.md).


### Enabling logs

On Tizen platform Netcoredbg will send logs to the system logger. On other platforms you should specify the file to which logs will be written. This can be done by setting environment variable, example:
```
export  LOG_OUTPUT=/tmp/log.txt
```

Each line of the log lines has same format which is described below:
```
5280715.183 D/NETCOREDBG(P12036, T12036): cliprotocol.cpp: evalCommands(1309) > evaluating: 'source file.txt'
      ^     ^  ^          ^       ^        ^               ^            ^       ^
      |     |  |          |       |        |               |            |       `-- Message itself.
      |     |  |          |       |        |               |            |   
      |     |  |          |       |        |               |            `-- Source line number.
      |     |  |          |       |        |               |    
      |     |  |          |       |        |               `-- This is function name.
      |     |  |          |       |        |
      |     |  |          |       |        `-- This is file name in which logging is performed.
      |     |  |          |       |
      |     |  |          |       `-- This is thread ID.
      |     |  |          |      
      |     |  |          `-- This is process PID
      |     |  |         
      |     |  `-- This program name (always NETCOREDBG).
      |     |
      |     `-- This is log level: E is for error, W is for warnings, D is for debug...
      |
      `--- This is time in seconds from the boot time (might be wrapped around).
```


### Selecting between Debug and Release builds

You can select build type by providing one of the following options for CMake:

  * ` -DCMAKE_BUILD_TYPE=Debug ` for debug build (no optimizations, suitable for debugging);

  * ` -DCMAKE_BUILD_TYPE=Release ` for release builds (optimized, hard to debug).

By default build system create release builds.


### Using address sanitizer

Example:
```
CC=clang-10 CXX=clang++-10 cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin  -DCMAKE_BUILD_TYPE=Debug  -DCORECLR_DIR=/path/to/coreclr -DDOTNET_DIR=/usr/share/dotnet -DASAN=1
```


### Using clang-tidy

Install clang-10. To use clang-tidy modify command used to configure the build:

```
CC=clang-10 CXX=clang++-10   cmake .. . -DCMAKE_CXX_CLANG_TIDY=clang-tidy-10 -DCMAKE_INSTALL_PREFIX=$PWD/../bin
```

Then just run `make`. All errors will be printed to stderr.

See details here: https://blog.kitware.com/static-checks-with-cmake-cdash-iwyu-clang-tidy-lwyu-cpplint-and-cppcheck/

Note: clang-analyzer (scan-build), cpplint, cppcheck, iwyu -- these tools currently will not work with Netcoredbg sources due to miscellaneous problems.

