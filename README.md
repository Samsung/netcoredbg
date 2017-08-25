# Managed code debugger for CoreCLR

This is the repo for Tizen build of managed code debugger for CoreCLR called `netcoredbg`.

The debugger sources are located in https://github.sec.samsung.net/i-kulaychuk/coreclr/tree/debugger

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

   Clone the repo, run `init.sh` script to download debugger sources and build as usual:
   ```
       ./init.sh
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
