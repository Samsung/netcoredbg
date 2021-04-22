// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file interop_win32.h  This file contains windows-specific functions for Interop class defined in interop.h

#ifdef WIN32
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <set>

#include "managed/interop.h"
#include "filesystem.h"

namespace netcoredbg
{

// This function searches *.dll files in specified directory and adds full paths to files
// to semicolon-separated list `tpaList'.
template <>
void InteropTraits<Win32PlatformTag>::AddFilesFromDirectoryToTpaList(const std::string &directory, std::string& tpaList)
{
    const char * const tpaExtensions[] = {
        "*.ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
        "*.dll",
        "*.ni.exe",
        "*.exe",
    };

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        size_t extLength = strlen(ext);

        std::string assemblyPath(directory);
        assemblyPath += FileSystem::PathSeparator;
        assemblyPath.append(tpaExtensions[extIndex]);

        WIN32_FIND_DATAA data;
        HANDLE findHandle = FindFirstFileA(assemblyPath.c_str(), &data);

        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {

                    std::string filename(data.cFileName);
                    size_t extPos = filename.length() - extLength;
                    std::string filenameWithoutExt(filename.substr(0, extPos));

                    // Make sure if we have an assembly with multiple extensions present,
                    // we insert only one version of it.
                    if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
                    {
                        addedAssemblies.insert(filenameWithoutExt);

                        tpaList.append(directory);
                        tpaList += FileSystem::PathSeparator;
                        tpaList.append(filename);
                        tpaList.append(";");
                    }
                }
            }
            while (0 != FindNextFileA(findHandle, &data));

            FindClose(findHandle);
        }
    }
}

// This function unsets `CORECLR_ENABLE_PROFILING' environment variable.
template <>
void InteropTraits<Win32PlatformTag>::UnsetCoreCLREnv()
{
    _putenv("CORECLR_ENABLE_PROFILING=");
}

// Returns the length of a BSTR.
template <>
UINT InteropTraits<Win32PlatformTag>::SysStringLen(BSTR bstrString)
{
    return ::SysStringLen(bstrString);
}

}  // ::netcoredbg
#endif  // WIN32
