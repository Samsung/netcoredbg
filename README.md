# Debugger for .NET Core runtime

The debugger provides GDB/MI or VSCode debug adapter interface and allows to debug .NET apps under .NET Core runtime.


## Build (Ubuntu x64)

1. Install .NET Core SDK 2.0+ from https://dot.net/core

2. Build coreclr, see https://github.com/dotnet/coreclr for instructions

3. Build the debugger with `cmake` and `clang` (assuming current directory is project root, coreclr is cloned and built next to debugger directory):
   ```
   mkdir build
   cd build

   CC=clang CXX=clang++ cmake .. -DCMAKE_INSTALL_PREFIX=$PWD/../bin

   make -j
   make install
   ```

   CMake accepts additional options:

   `-DCLR_DIR=$HOME/git/coreclr` path to coreclr source root directory

   `-DCLR_BIN_DIR=$HOME/git/coreclr/bin/Product/Linux.x64.Debug` path to coreclr build result directory

4. The above commands create `./bin` directory with `netcoredbg` binary and additional libraries.

   Now running the debugger with `--help` option should look like this:
   ```
   $ ../bin/netcoredbg --help
   .NET Core debugger for Linux/macOS.

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
