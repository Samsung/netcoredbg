// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem_win32.cpp
/// This file contains definitions of windows-specific functions related to file system.

#ifdef WIN32
#include <windows.h>
#include <string>
#include "filesystem.h"

namespace netcoredbg
{

const char* FileSystemTraits<Win32PlatformTag>::PathSeparatorSymbols = "/\\";

// Function returns absolute path to currently running executable.
std::string GetExeAbsPath()
{
    const size_t MAX_LONGPATH = 1024;
    char hostPath[MAX_LONGPATH + 1];
    static const std::string result(hostPath, ::GetModuleFileNameA(NULL, hostPath, MAX_LONGPATH));
    return result;
}


// Function returns path to directory, which should be used for creation of
// temporary files. Typically this is `/tmp` on Unix and something like
// `C:\Users\localuser\Appdata\Local\Temp` on Windows.
string_view GetTempDir()
{
    CHAR path[MAX_PATH + 1];
    static const std::string result(path, GetTempPathA(MAX_PATH, path));
    return result;
}


// Function changes current working directory. Return value is `false` in case of error.
bool SetWorkDir(string_view path)
{
    char str[MAX_PATH];
    if (path.size() >= sizeof(str))
        return false;

    path.copy(str, path.size());
    str[path.size()] = 0;
    return SetCurrentDirectoryA(str);
}

}  // ::netcoredbg
#endif
