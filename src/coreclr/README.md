This directory and files within this directory needed to resolve file name
and identifier names clashing between CoreCLR and C/C++ libraries.

In future (after dropping support of CoreCLR2.1) this directory might be
removed and following directories should be added to global list of include-files
directories: `${CORECLR_SRC_DIR}/pal/inc`, `${CORECLR_SRC_DIR}/pal/inc/rt`,
`${CORECLR_SRC_DIR}/inc`, `${CORECLR_SRC_DIR}/debug/inc`.

Originally few issues was exist with CoreCLR 2.1:

1) `specstrings.h` from CoreCLR defines `__deref` macros, which conflicts with
   some C++ code from `<functional>` (in case, when `_GLIBCXX_DEBUG` is defined);

2) `pal.h` from CoreCLR redefines few standard macroses from limits.h;

3) originally, CoreCLR source files tree was added to list of include directories,
   and `${CORECLR_SRC_DIR}src/pal/inc/rt` contains include-files with such
   names, that it clashes standard include files (especially, `assert.h`);

4) `pal.h` declares own version of mathematic functions with different
   exception specification (this brokes build with clang version >= 10).

To solve issues 1, 2 and 3 this directory (`src/coreclr`) was created, which allows to
include few coreclr files without file name clashing.  And coreclr's paths was
removed from global include directories list (from `src/CMakeLists.txt`).

Issue 2 also fixed by ignoring warnings and redefinig macros back by including
`limits.h` after inclusion of `pal.h`.

Issue 4 can be fixed, if system `math.h` included before any of include
file belonging to coreclr.

