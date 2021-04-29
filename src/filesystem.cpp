// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file filesystem.cpp
/// This file contains definitions of cross-platform functions related to file system.

#include "filesystem.h"

namespace netcoredbg
{

template <> const char* FileSystemImpl<FileSystemTraits<PlatformTag> >::PathSeparatorSymbols = 
                            FileSystemTraits<PlatformTag>::PathSeparatorSymbols;

// Function returns only file name part of the full path.
std::string GetBasename(const std::string &path)
{
    size_t i = path.find_last_of(FileSystem::PathSeparatorSymbols);
    return i == std::string::npos ? path : path.substr(i + 1);
}

// Function checks, if given path contains directory names (strictly speaking,
// contains path separator) or consists only of a file name. Return value is `true`
// if argument is not the file name, but the path which includes directory names.
bool IsFullPath(const std::string &path)
{
    size_t pos = path.find_last_of(FileSystem::PathSeparatorSymbols);
    return pos != std::string::npos;
}

}  // ::netcoredbg
