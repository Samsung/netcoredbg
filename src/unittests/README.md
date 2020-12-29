# How to use unit tests.

1) Configure and build project (see top level README.md);

2) in `build` directory give command `make test`.

If some tests failed: you will see it, example:

```
netcoredbg/build$ make test
Running tests...
Test project /home/sysop/netcoredbg/build
    Start 1: iosystem
1/1 Test #1: iosystem .........................***Exception: SegFault  0.01 sec

0% tests passed, 1 tests failed out of 1

Total Test time (real) =   0.01 sec

The following tests FAILED:
          1 - iosystem (SEGFAULT)
Errors while running CTest
Makefile:119: recipe for target 'test' failed
make: *** [test] Error 8
```

To see details, run single test:

```
netcoredbg/build$ ./src/unittests/iosystem

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
iosystem is a Catch v2.6.1 host application.
Run with -? for options

-------------------------------------------------------------------------------
IOSystem::basic
-------------------------------------------------------------------------------
/home/sysop/netcoredbg/src/unittests/iosystem_test.cpp:17
...............................................................................

/home/sysop/netcoredbg/src/unittests/iosystem_test.cpp:17: FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal

===============================================================================
test cases: 1 | 1 failed
assertions: 5 | 4 passed | 1 failed

Segmentation fault
```

Now you can debug separate test with gdb:

```
netcoredbg/build$ gdb ./src/unittests/iosystem
GNU gdb (Ubuntu 8.1-0ubuntu3.2) 8.1.0.20180409-git
...
Reading symbols from ./src/unittests/iosystem...done.
(gdb) run
Starting program: /home/sysop/netcoredbg/build/src/unittests/iosystem

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
iosystem is a Catch v2.6.1 host application.
Run with -? for options

-------------------------------------------------------------------------------
IOSystem::basic
-------------------------------------------------------------------------------
/home/sysop/netcoredbg/src/unittests/iosystem_test.cpp:17
...............................................................................

/home/sysop/netcoredbg/src/unittests/iosystem_test.cpp:27: FAILED:
  CHECK( fdset.isset(std::get<IOSystem::Stdin>(std_files)) == false )
with expansion:
  true == false


Program received signal SIGSEGV, Segmentation fault.
0x00000000005a50e2 in netcoredbg::IOSystemTraits<netcoredbg::UnixPlatformTag>::FDSet::isset (this=0x7fffffffd5f0,
    fh=...) at /home/sysop/netcoredbg/src/unix/iosystem_unix.h:38
38              bool isset(const FileHandle& fh) const { return FD_ISSET(fh.fd, &set); }
(gdb)

```
