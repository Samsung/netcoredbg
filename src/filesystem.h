// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem.h
/// This file contains declaration related to file system, which profide unified
/// platform-independent interface to some basic filesystem related operations.

#pragma once
#include <cstddef>
#include "string_view.h"
#include "platform.h"

namespace netcoredbg
{
    using Utility::string_view;

    /// This is platform-specific traits class, which implements
    /// some operations in platform-specific way.
    template <typename Platform> struct FileSystemTraits {};

    /// This class defines platform-specific details.
    /// These definitions should be accessible via `FileSystem` class (not FileSystemImpl).
    template <typename Traits>
    struct FileSystemImpl
    {
        /// Maximum allowed length of a full path, in characters, including terminal null.
        const static size_t PathMax = Traits::PathMax;

        /// Maximum allowed length of a file name only (characters, not including terminal null).
        const static size_t NameMax = Traits::NameMax;

        /// Symbol used to separate directories and file name.
        const static char PathSeparator = Traits::PathSeparator;

        /// C-String which contains all possible symbols which can be used as path separator.
        const static char* PathSeparatorSymbols;
    };

    /// Function returns absolute path to currently running executable.
    std::string GetExeAbsPath();

    /// Function returns only file name part of the full path.
    string_view GetBasename(string_view path);

    /// Function returns directory in which resides file or directory specified as an argument.
    string_view GetDirname(string_view path);

    /// Function changes current working directory. Return value is `false` in case of error.
    bool SetWorkDir(string_view path);

    /// Function returns path to directory, which should be used for creation of
    /// temporary files. Typically this is `/tmp` on Unix and something like
    /// `C:\Users\localuser\Appdata\Local\Temp` on Windows.
    string_view GetTempDir();

    /// Function checks, if given path contains directory names (strictly speaking,
    /// contains path separator) or consists only of a file name. Return value is `true`
    /// if argument is not the file name, but the path which includes directory names.
    bool IsFullPath(string_view path);

}  // ::netcoredbg

#include "filesystem_win32.h"
#include "filesystem_unix.h"

namespace netcoredbg
{
    using FileSystem = FileSystemImpl<FileSystemTraits<PlatformTag> >;
}
