# F# Computation Expression Breakpoint Bug — netcoredbg

## Bug

Line breakpoints inside F# computation expression (CE) blocks don't fire. The breakpoint silently slides to the nearest non-closure sequence point (typically the module initializer).

This affects all F# CE-heavy frameworks: Expecto, FAKE, Saturn, Giraffe, etc.

## Root cause

The F# compiler transforms CE bodies into closure classes. For example:

```fsharp
let ceTests =
    test "ce breakpoint test" {
        let a = 100              // Line 17 — user wants breakpoint here
        ...
    }
```

The PDB contains correct sequence points for the closure method `ceTests@17.Invoke` at lines 17-20. However, netcoredbg's `GetMethodTokensByLineNumber()` only searches the outer method's range, misses the closure class entirely, and slides the breakpoint to the nearest available sequence point in the static constructor (line 15).

## Reproduction

### Prerequisites

- .NET 8 SDK
- netcoredbg (any recent version; tested with 3.1.3)

### Build

```bash
cd FSharpCeBreakpointRepro
dotnet build -c Debug
```

### Test — DAP mode (shows the bug)

```bash
python3 test-dap.py
```

Expected output:
```
Line  8 (regular function): HIT
Line 17 (CE block body):    MISSED
Line 15 (CE module init):   HIT (bp slid from line 17 to 15)
```

### Test — CLI mode (same bug, previously thought to work)

```bash
bash test-cli.sh
```

Shows the same behavior: `break Program.fs:17` resolves to line 15 (`.cctor()`), not the CE body.

### PDB verification

The closure class has correct sequence points:

```
=== ceTests@17.Invoke (0x0600000D) ===
  IL_0000  Program.fs:17 (col 9-20)
  IL_0003  Program.fs:18 (col 9-22)
  IL_0007  Program.fs:19 (col 9-36)
  IL_0025  Program.fs:20 (col 9-45)
```

The data is in the PDB — the debugger's method token enumeration just doesn't find it.

## Environment

- macOS arm64 (Darwin 25.3.0)
- .NET SDK 8.0.201
- F# compiler: included in .NET SDK 8.0
- netcoredbg 3.1.3 (built from source)
- Expecto 10.2.3

## Fix direction

`GetMethodTokensByLineNumber()` in `modules_sources.cpp` needs to search closure/nested class methods when the target line falls within a closure's PDB sequence point range. The closure classes are present in the metadata — they have entries in `IMetaDataImport::EnumNestedClasses()` — but the current enumeration doesn't include them in the method range search.
