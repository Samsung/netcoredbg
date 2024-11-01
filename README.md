# Debugger for the .NET Core Runtime

The NetCoreDbg debugger implements [GDB/MI](https://sourceware.org/gdb/onlinedocs/gdb/GDB_002fMI.html)
and [VSCode Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/) in a unified framework, allowing the debugging of .NET apps under the .NET Core runtime as well as facilitating debugging from the command line (such as in GDB).

## Copyright

You can find licensing information in the [LICENSE](LICENSE) file within the root directory of the NetCoreDbg sources.

## Usage
Further details regarding the usage of NetCoreDbg can be found in the [CLI](docs/cli.md) manual.

## Installation
NetCoreDbg is available in repositories of some Linux distributions and Windows package manager:
- Arch Linux (https://aur.archlinux.org/packages/netcoredbg)
- Gentoo Linux (https://packages.gentoo.org/packages/dev-dotnet/netcoredbg)
- LiGurOS (https://gitlab.com/liguros/liguros-repo/-/tree/develop/dev-dotnet/netcoredbg?ref_type=heads)
- NixOS (https://mynixos.com/nixpkgs/package/netcoredbg)
- Scoop (Windows) (https://github.com/ScoopInstaller/Main/blob/master/bucket/netcoredbg.json)

For other Linux distributions you can use binaries provided in github releases (https://github.com/Samsung/netcoredbg/releases) or build from source code.

## Building from Source Code

Currently, NetCoreDbg can be built on Linux, MacOS, or Windows. Instructions for building NetCoreDbg on each platform is shown below.

### Supported Architectures

- ARM 32-bit
- ARM 64-bit
- x64
- x86
- RISC-V 64-bit

### Unix

NetCoreDbg's build requires Microsoft's .NET, and as such, NetCoreDbg can only be built in Linux. Microsoft supports a few distributions, the details of which can be found here: https://docs.microsoft.com/en-us/dotnet/core/install/linux.

#### Prerequisites

1. You need to install `cmake`, and either `make` or `ninja`.

2. You need the clang C++ compiler installed (NetCoreDbg can't be built with gcc).

3. Microsoft's **.NET runtime** should be installed, which you can download here: https://dotnet.microsoft.com/download.

4. You may also need to install some common developers tools not mentioned here, such as [Git](https://www.git-scm.com/downloads), etc...

5. It is expected that you place the NetCoreDbg sources within a directory.

6. Optional step: NetCoreDbg requires the **CoreCLR runtime source code**, which is typically downloaded automatically, but can also be downloaded manually from here: https://github.com/dotnet/runtime.

   *For example, you can check out tag v8.x.*

7. Optional step: NetCoreDbg requires the **.NET SDK**, which is typically downloaded automatically, but can also be downloaded manually from here: https://dotnet.microsoft.com/download.

#### Compiling

Configure the build with the following commands:

```
user@netcoredbg$ mkdir build
user@netcoredbg$ cd build
user@build$ CC=clang CXX=clang++ cmake ..
```

In order to run tests after a successful build, you need to add the option `-DCMAKE_INSTALL_PREFIX=$PWD/../bin`.

To enable the Source-Based Code Coverage feature (https://clang.llvm.org/docs/SourceBasedCodeCoverage.html),
add the `-DCLR_CMAKE_ENABLE_CODE_COVERAGE` option.

If you have previously downloaded the .NET SDK or CoreCLR sources, then you should modify the command line by adding the following options: 
`-DDOTNET_DIR=/path/to/sdk/dir -DCORECLR_DIR=/path/to/coreclr/sources`.

If cmake tries to download the .NET SDK or CoreCLR sources and fails, then please see bullet numbers 6 and 7 above. *You can download any required files manually*.

After configuration has finished, you can then build NetCoreDbg:

```
user@build$ make
...
user@build$ make install
```

To perform a build from scratch, including the configuration step, you should again delete any artifacts with the following commands:

```
user@build$ cd ..
user@netcoredbg$ rm -rf build src/debug/netcoredbg/bin bin
```

> *Directory `bin` contains "installed" NetCoreDbg's binaries for tests. If you have installed NetCoreDbg in other places, for example in `/usr/local/bin`, you should remove it manually because NetCoreDbg's build system doesn't currently implement automatic uninstalling.*

#### Prerequisites and Compiling with Interop Mode Support (Linux and Tizen OSes only)
The prerequisites and compiling process are the same as the aforementioned with the following changes:

1. Depending on your distro, you need to install either the `libunwind-dev` or `libunwind-devel` packages.

2. Configure the build with the following commands:

```
user@build$ CC=clang CXX=clang++ cmake .. -DINTEROP_DEBUGGING=1
```
To find more details on the usage of NetCoreDbg in Interop Mode, please see the guide doc: [Interop Mode](docs/interop.md).

### MacOS

You need to install homebrew from here: https://brew.sh/

After this, the build instructions are the same as for Unix, including the prerequisites.

*Note: the MacOS arm64 build (M1) is community supported and may not work as expected, as well as some tests possibly failing.*

### Windows

#### Prerequisites:

1. Download and install **CMake** from here: https://cmake.org/download

2. Download and install **Microsoft's Visual Studio 2019** or newer from here: https://visualstudio.microsoft.com/downloads

   *During installation of Visual Studio you should install all of the options required
   for C# and C++ development on Windows*.

3. Download and install **Git**; you have a few options here:

 * use original Git: https://git-scm.com/download/win
 * use TortoiseGit: https://tortoisegit.org/download
 * or use git installed in cygwin: https://cygwin.com/install.html

4. Utilize Git to place NetCoreDbg sources in a directory.

5. This step may be omitted, and in that case, cmake will automatically download all necessary files.
   But if it fails, you then need to manually download the **CoreCLR sources** into another directory from here: https://github.com/dotnet/runtime.

   *For example, you can use the latest tag **v8.x***.

6. This step may also be omitted, and in that case, cmake will automatically download all necessary files.
   But if it fails, you then need to manually download and install the **.NET SDK** from here: https://dotnet.microsoft.com/download

#### Compiling

Configure the build with the following commands given in NetCoreDbg's source tree:

```
C:\Users\localuser\netcoredbg> md build
C:\Users\localuser\netcoredbg> cd build
C:\Users\localuser\netcoredbg\build> cmake .. -G "Visual Studio 16 2019"
```

*Note: You should run this command from cmd.exe, **not from cygwin's shell***.

Option `-G` specifies which instance of Visual Studio should build the project.
Note, the minimum requirements for NetCoreDbg's build is the **Visual Studio 2019** version.

If you want to run tests after a successful build, then you should add the following option: `-DCMAKE_INSTALL_PREFIX="%cd%\..\bin"`.

If you have downloaded either the .NET SDK or .NET Core sources manually, you should add the following options:
`-DDOTNET_DIR="c:\Program Files\dotnet" -DCORECLR_DIR="path\to\coreclr"`

To compile and install, use the following command:

```
C:\Users\localuser\netcoredbg\build> cmake --build . --target install
```

To perform a build from scratch, including the configuration step, you should again delete any artifacts by using the following commands:

```
C:\Users\localuser\netcoredbg\build>cd ..
C:\Users\localuser\netcoredbg>rmdir /s /q build src\debug\netcoredbg\bin bin
```

> *Directory `bin` contains the "installed" NetCoreDbg's binaries for tests. If you have installed NetCoreDbg in other places, you should remove it manually because NetCoreDbg's build system doesn't currently perform automatic uninstalling.*

## Running NetCoreDbg

In the instructions provided above, the `netcoredbg` binary and additional libraries will be installed in some directory.
For development purposes (for running tests, debugging, etc...), the directory `bin` in NetCoreDbg's source tree is typically used.

Now, running the debugger with the `--help` option should look like this:

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

Basically, to debug .NET code, you should run NetCoreDbg with the following command line:

```
$ /path/to/netcoredbg --interpreter=TYPE -- /path/to/dotnet /path/to/program.dll
```

## Notes for Developers

### Running the Tests

Detailed instructions on how to run tests can be found in the `test-suite` directory here: [test-suite/README.md](test-suite/README.md).
You simply need to build and install NetCoreDbg into the `bin` directory (in the NetCoreDbg source tree) and then change the directory to `test-suite` and run the following script `/run_tests.sh`.

If you wish to get the "Source-Based Code Coverage" report, you can add a `-c` or `--coverage` option to the command line, i.e.:
`./run_tests.sh -c [[testname1][testname2]..]`.

Please note, for that case your build configuration should be implemented with the `-DCLR_CMAKE_ENABLE_CODE_COVERAGE` option (please see above). *This feature is currently only supported on Unix-like platforms*.

### Building and Running Unit Tests

In order to build unit tests, you need to add the following option to CMake: `-DBUILD_TESTING=ON`.

After a successful build, you may then run unit tests by running the command: `make test`.

Please see details here: [src/unittests/README.md](src/unittests/README.md).

### Enabling Logs

On the Tizen platform, NetCoreDbg will send logs to the system logger. On other platforms, you should specify the file to which any logs will be written. This can be done by setting the environment variable, for example:
```
export LOG_OUTPUT=/tmp/log.txt
```

Each line of the log file utilizes the same format which is explained below:
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

### Selecting between Debug and Release Builds

You can select the build type by providing one of the following options for CMake:

  * ` -DCMAKE_BUILD_TYPE=Debug ` for a debug build (contains zero optimizations, but is suitable for debugging);

  * ` -DCMAKE_BUILD_TYPE=Release ` for a release build (optimized, but difficult to debug).

By default, NetCoreDbg's build system creates release builds.

### Using the Address Sanitizer

Example:
```
CC=clang-10 CXX=clang++-10 cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin  -DCMAKE_BUILD_TYPE=Debug  -DCORECLR_DIR=/path/to/coreclr -DDOTNET_DIR=/usr/share/dotnet -DASAN=1
```

### Using Clang-Tidy

First, Install clang-10.

Next, to use clang-tidy, modify the commands used to configure the build as below:

```
CC=clang-10 CXX=clang++-10   cmake .. . -DCMAKE_CXX_CLANG_TIDY=clang-tidy-10 -DCMAKE_INSTALL_PREFIX=$PWD/../bin
```

Then just run `make`. Any and all errors will be printed to stderr.

See details here: https://blog.kitware.com/static-checks-with-cmake-cdash-iwyu-clang-tidy-lwyu-cpplint-and-cppcheck/

*Note: Due to miscellaneous problems, the following tools currently will not work with NetCoreDbg: clang-analyzer (scan-build), cpplint, cppcheck, and iwyu*.

