// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#pragma once

namespace netcoredbg
{

namespace BuildInfo
{
    extern const char version[];    // version for displaying
    extern const char build_type[]; // build type (same version might have different build types)

    // Following two strings contain GIT revision hash, tag, SVN revision number, etc...
    extern const char netcoredbg_vcs_info[];  // for netcoredbg itself
    extern const char coreclr_vcs_info[];     // for CoreCLR (which might come from different source tree)

    extern const char os_name[];   // OS name for which project was build.
    extern const char cpu_arch[];  // CPU architecture name for which project was build.

    extern const char date[];
    extern const char time[];      // Date and time of the build.

    extern const char hostname[];  // hostname of the buildserver.
}


} // namespace netcoredbg
