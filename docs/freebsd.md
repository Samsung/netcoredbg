# FreeBSD build and launch test

The following procedure was validated on FreeBSD 15.1 amd64 with the FreeBSD
packages for .NET SDK/runtime 9.0.115/9.0.14, CMake 3.31 and LLVM 19. Build as
an ordinary user.

NetCoreDbg requires CoreCLR headers and a `libdbgshim.so` built from a matching
dotnet/diagnostics source revision. The dbgshim PAL must include
[dotnet/diagnostics#6183](https://github.com/dotnet/diagnostics/pull/6183) or an
equivalent FreeBSD process-module implementation; otherwise runtime startup
cannot locate `libmscordbi.so`.

```sh
cmake -S diagnostics -B diagnostics-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/local/llvm19/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/local/llvm19/bin/clang++
cmake --build diagnostics-build --target dbgshim -j2

cmake -S netcoredbg -B netcoredbg-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOTNET_DIR=/usr/local/share/dotnet \
  -DCORECLR_DIR=/path/to/matching-runtime/src/coreclr \
  -DDBGSHIM_DIR="$PWD/diagnostics-build/src/dbgshim"
cmake --build netcoredbg-build -j2
```

Create a `net9.0` console project named `debug-smoke` with portable debug
symbols. Put `// BREAKPOINT` on an executable line in `Program.cs`, build it,
then run the dependency-free DAP test:

```sh
dotnet build debug-smoke.csproj
python3 netcoredbg/test-suite/freebsd_dap_smoke.py \
  netcoredbg-build/src/netcoredbg \
  /path/to/debug-smoke
```

Success requires a launch response, a stop at the marked source line, a
successful continue, an `exited` event with status zero and a `terminated`
event. A successful build or DAP initialization alone is not acceptance.
