// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file platform_win32.cpp  This file contains windows-specific function definitions,
/// for functions defined in platform.h

#ifdef WIN32
#include <windows.h>
#include <stdlib.h>  // char **environ
#include "platform.h"

namespace netcoredbg
{

// Function returns memory mapping page size (like sysconf(_SC_PAGESIZE) on Unix).
unsigned long OSPageSize()
{
    static unsigned long pageSize = []{
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            return si.dwPageSize;
        }();

    return pageSize;
}


// Function suspends process execution for specified amount of time (in microseconds)
void USleep(unsigned long usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10*(long)usec); // Convert to 100 nanosecond interval, negative value indicates relative time

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}


// Function returns list of environment variables (like char **environ).
char** GetSystemEnvironment()
{
    return environ;
}

}  // ::netcoredbg
#endif
