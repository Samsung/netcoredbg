// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem_unix.cpp
/// This file contains definitions of unix-specific functions related to file system.

#ifdef __unix__
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
    static span<char> get_exe_path(span<char> buffer)
    {
        static const char* self_link = "/proc/self/exe";
        ssize_t r = readlink(self_link, buffer.data(), buffer.size());
        if (r < 0 || size_t(r) >= buffer.size())
            return {};  // error or path too long

        buffer[r] = 0;
        return buffer.subspan(0, r);
    }

#elif defined(__APPLE__)
    static span<char> get_exe_path(span<char> buffer)
    {
        uint32_t real_len;
        if (_NSGetExecutablePath(buffer.data(), &real_len) < 0)
            return {};

        if (real_len >= buffer.size())
            return {};

        buffer[real_len] = 0;
        return buffer.subspan(0, real_len);
    }
#endif
}


// Function returns absolute path to currently running executable.
string_view GetExeAbsPath()
{
    std::array<char, PATH_MAX> exe_path;
    auto path = get_exe_path(exe_path);
    static const std::string result {path.data(), path.size()};
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
bool SetWorkDir(string_view path)
{
    char str[PATH_MAX];
    if (path.size() >= sizeof(str))
        return false;

    path.copy(str, path.size());
    str[path.size()] = 0;
    return chdir(str) == 0;
}

}  // ::netcoredbg
#endif __unix__
