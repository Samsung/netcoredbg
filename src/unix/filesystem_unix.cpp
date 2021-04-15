// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem_unix.cpp
/// This file contains definitions of unix-specific functions related to file system.

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#include <array>
#include <string>
#include "filesystem.h"
#include "string_view.h"
#include "span.h"
#include "limits.h"

namespace netcoredbg
{

using Utility::string_view;
template <typename T> using span = Utility::span<T>;

const char* FileSystemTraits<UnixPlatformTag>::PathSeparatorSymbols = "/";

namespace
{
#ifdef __linux__
    std::string get_exe_path()
    {
        static const char self_link[] = "/proc/self/exe";
        char buffer[PATH_MAX];
        ssize_t r = readlink(self_link, buffer, PATH_MAX);
        return std::string(buffer, r < 0 ? 0 : r);
    }
#elif defined(__APPLE__)
    std::string get_exe_path()
    {
        uint32_t lenActualPath = 0;
        if (_NSGetExecutablePath(nullptr, &lenActualPath) == -1)
        {
            // OSX has placed the actual path length in lenActualPath,
            // so re-attempt the operation
            std::string resizedPath(lenActualPath, '\0');
            char *pResizedPath = const_cast<char *>(resizedPath.data());
            if (_NSGetExecutablePath(pResizedPath, &lenActualPath) == 0)
                return pResizedPath;
        }
        return std::string();
    }
#endif
}

// Function returns absolute path to currently running executable.
std::string GetExeAbsPath()
{
    static const std::string result(get_exe_path());
    return result;
}

// Function returns path to directory, which should be used for creation of
// temporary files. Typically this is `/tmp` on Unix and something like
// `C:\Users\localuser\Appdata\Local\Temp` on Windows.
string_view GetTempDir()
{
    auto get_tmpdir = []()
    {
        const char *pPath = getenv("TMPDIR");
        if (pPath != nullptr)
            return pPath;
        else
            return P_tmpdir;
    };

    static const std::string result {get_tmpdir()};
    return result;
}


// Function changes current working directory. Return value is `false` in case of error.
bool SetWorkDir(const std::string &path)
{
    return chdir(path.c_str()) == 0;
}

}  // ::netcoredbg
#endif __unix__
