// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file platform_unix.cpp  This file contains unix-specific function definitions,
/// for functions defined in platform.h

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#include <unistd.h>
#include "utils/platform.h"

extern char** environ;

namespace netcoredbg
{

// Function returns memory mapping page size (like sysconf(_SC_PAGESIZE) on Unix).
unsigned long OSPageSize()
{
    static unsigned long pageSize = sysconf(_SC_PAGESIZE);
    return pageSize;
}


// Function suspends process execution for specified amount of time (in microseconds)
void USleep(unsigned long usec)
{
    usleep(usec);
}


// Function returns list of environment variables (like char **environ).
char** GetSystemEnvironment()
{
#if __APPLE__
    return *(_NSGetEnviron());
#else   // __APPLE__
    return environ;
#endif  // __APPLE__
}

}  // ::netcoredbg
#endif  // __unix__
