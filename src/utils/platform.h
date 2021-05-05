// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

/// \file platform.h  This file contains platform-specific (Windows/Unix) definitions.

#pragma once

// TODO replace in code, remove this macros
#ifdef _MSC_VER
#define W(s) L##s
#else
#define W(s) u##s
#endif

/// @{ PLATFORM_TAG is the macros, which defines a platform, for which code is currently compiling,
/// macros value should expand to C++ type, which defines platform too (see below).
#ifdef WIN32
#define PLATFORM_TAG Win32PlatformTag
#else
#define PLATFORM_TAG UnixPlatformTag
#endif
/// @}

namespace netcoredbg
{

    struct Win32PlatformTag {};  /// PlatformTag for Windows (see below)
    struct UnixPlatformTag {};   /// PlatformTAg for Unix and MacOS.
   
    /// PlatformTag is the type, which determines platform, for which code is currenly compilih.
    /// This tag might be used to select proper template specialization.
    using PlatformTag = PLATFORM_TAG;

    /// Function returns memory mapping page size (like sysconf(_SC_PAGESIZE) on Unix).
    unsigned long OSPageSize();

    /// Function suspends process execution for specified amount of time (in microseconds)
    void USleep(unsigned long usec);

    /// Function returns list of environment variables (like char **environ).
    char** GetSystemEnvironment();

} // ::netcoredbg
