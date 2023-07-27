# Using NetCoreDbg with interop mode

Debug in interop mode or mixed native/managed mode (C#, C, C++).
There are two interop mode debugging scenarios:
 - "native" app calls "managed" DLL (supported debug session type: attach).
 - "managed" app calls "native" dynamic library (supported debug session type: launch or attach).

## Restrictions

**Interop mode is implemented for Linux and Tizen OSes only for amd64, x86, arm64 and arm32 architectures.**

Since debugger uses CoreCLR Debug API, debug of CoreCLR itself is not allowed.
This means that user can't stop at breakpoint, break, step, handle signal or exception in CoreCLR related dynamic libraries:
- libclrjit.so
- libcoreclr.so
- libcoreclrtraceptprovider.so
- libhostpolicy.so
- System.Globalization.Native.so
- System.Security.Cryptography.Native.OpenSsl.so
- System.IO.Compression.Native.so
- System.Net.Security.Native.so
- System.Native.so
- System.Net.Http.Native.so
- libSystem.Native.so
- libSystem.IO.Compression.Native.so
- libSystem.Net.Security.Native.so
- libSystem.Security.Cryptography.Native.OpenSsl.so
- libSystem.Globalization.Native.so
- libclrgc.so

All native events will be forwarded to debuggee process runtime for handling.

## Start debug session

To start debugger in interop mode and attach to process:
```
$ netcoredbg --interpreter=cli --interop-debugging --attach PID
```

To start debugger and run program with one command:
```
$ netcoredbg --interpreter=cli --interop-debugging -- dotnet hello.dll param1 param2
```

## Current interop mode status

### Supported

- Source line breakpoints.
- Native and mixed (managed and native) code backtraces.
- User code breaks with `__builtin_debugtrap()` and `__builtin_trap()`.
- User native code threads stop at native/manged events (Note, `libc` and `libstdc++` debug info must be provided).

### Not supported

- Function breakpoints.
- Function and line breakpoints conditions.
- Stepping through code lines (native, native-managed and managed-native).
- Stepping through native code instructions.
- Signal handling (all signals handled by managed runtime now).
- Exception handling.
- Exception breakpoints.
- Evaluation.
