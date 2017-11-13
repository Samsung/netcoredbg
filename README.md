# Managed code debugger for .NET Core runtime

The debugger provides GDB/MI interface and allows to debug .NET apps using ICorDebug API of CoreCLR.


## Build (Ubuntu x64)

1. Install .NET Core SDK 2.x from https://dot.net/core

2. Build and install coreclr and corefx, see https://github.com/dotnet/coreclr for details

3. Use the following script as a reference to build the debugger (assuming current directory is project root):
   ```
   #!/bin/sh

   # Path to coreclr source root
   CORECLR_PATH=$HOME/git/coreclr
   # Path to coreclr build output (use .Relese for release build)
   CORECLR_BIN=$CORECLR_PATH/bin/Product/Linux.x64.Debug
   # Path to generated coreclr overlay (where coreclr and corefx binaries are installed)
   CORECLR_OVERLAY=$HOME/git/overlay

   rm -rf build
   mkdir build
   cd build

   CC=clang CXX=clang++ cmake ../ -DCMAKE_INSTALL_PREFIX=$CORECLR_OVERLAY -DCLR_DIR=$CORECLR_PATH -DCLR_BIN_DIR=$CORECLR_BIN -DCLR_CMAKE_TARGET_ARCH_AMD64=1 -DCORECLR_SET_RPATH=\$ORIGIN
   make -j
   make install
   ```

   The script produces `netcoredbg` and `SymbolReader.dll` binaries inside the overlay directory.

## Build (GBS)

1. Prepare

   Prepare GBS environment and add a path to local repository to your `.gbs.conf`.

   See the guide here http://suprem.sec.samsung.net/confluence/display/SPTDTLC/Profiler+architecture

2. Build modified `coreclr` and `coreclr-devel` packages

   Apply the patches from `patches/coreclr` and build CoreCLR from
   https://review.tizen.org/gerrit/#/admin/projects/platform/upstream/coreclr

   This step will produce updated `coreclr` and `mscorlib` RPMs for installing on the device/emulator.
   Also it will generate the `coreclr-devel` package in the local repo which is necessary for building the debugger.

3. Build the `netcoredbg` package

   Clone the repo and build as usual:
   ```
   gbs build -A armv7l --include-all --spec netcoredbg.spec
   ```

4. Build modified `dotnet-launcher` package

   Apply the patches from `patches/dotnet-launcher` and build `dotnet-launcher` from
   https://review.tizen.org/gerrit/#/admin/projects/platform/core/dotnet/launcher

## Usage

1. Install packages on the device/emulator

   Install RPMs from local GBS repo: `coreclr`, `mscorlib`, `dotnet-launcher` and `netcoredbg`.

   You may also need to remove AOT images:
   ```
   sdb shell "find / -name '*.ni.dll' -exec rm {} \;"
   ```

   Reboot the device/emulator.

2. Use modified Visual Studio Tools for Tizen

   Build `vs-tools-cps` project from
   https://github.sec.samsung.net/i-kulaychuk/vs-tools-cps/tree/netcoredbg-attach
   and launch the debug session with F5.
