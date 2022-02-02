// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem_win32.h  This file contains windows-specific details to FileSystem class.

#ifdef WIN32
#pragma once
#include <cstddef>
#include <windows.h>

namespace netcoredbg
{
    template <> struct FileSystemTraits<Win32PlatformTag>
    {
        const static size_t PathMax = MAX_PATH;
        const static size_t NameMax = MAX_PATH - 1; // not include terminal null.
        const static char PathSeparator = '\\';
        const static char* PathSeparatorSymbols;
    };
} 
#endif
