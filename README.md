# Debugger for .NET Core runtime

The debugger provides GDB/MI or VSCode Debug Adapter protocol and allows to debug .NET apps under .NET Core runtime.


## Build

Build coreclr for your platform, see https://github.com/dotnet/coreclr for instructions.

Switch to `netcoredbg` directory, create `build` directory and switch into it:
```
mkdir build
cd build
```

Proceed to build with `cmake`.

> If `coreclr` and `netcoredbg` directories are next to each other, necessary `coreclr` files would be located automatically. Otherwise add `-DCLR_DIR=<path>/coreclr` to `cmake` command.

### Ubuntu

```
CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin
```

### macOS

```
cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin
```

### Windows

```
cmake .. -G "Visual Studio 15 2017 Win64" -DCMAKE_INSTALL_PREFIX="$pwd\..\bin"
```

Compile and install:
```
cmake --build . --target install
```


## Run

The above commands create `bin` directory with `netcoredbg` binary and additional libraries.

Now running the debugger with `--help` option should look like this:
```
$ ../bin/netcoredbg --help
.NET Core debugger

Options:
--attach <process-id>                 Attach the debugger to the specified process id.
--interpreter=mi                      Puts the debugger into MI mode.
--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.
--engineLogging[=<path to log file>]  Enable logging to VsDbg-UI or file for the engine.
                                      Only supported by the VsCode interpreter.
--server[=port_num]                   Start the debugger listening for requests on the
                                      specified TCP/IP port instead of stdin/out. If port is not specified
                                      TCP 4711 will be used.
```
