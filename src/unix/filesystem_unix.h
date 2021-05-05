// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem_unix.h  This file contains unix-specific details to FileSystem class.

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#pragma once
#include <cstddef>
#include <climits>
#include "utils/platform.h"

namespace netcoredbg
{
    template <> struct FileSystemTraits<UnixPlatformTag>
    {
        const static size_t PathMax = PATH_MAX;
        const static size_t NameMax = NAME_MAX;
        const static char PathSeparator = '/';
        const static char* PathSeparatorSymbols;
    };
} 
#endif
