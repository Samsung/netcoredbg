// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <sys/types.h>

namespace netcoredbg
{

#ifdef INTEROP_DEBUGGING
namespace InteropDebugging
{

    // Initialize interop debugging, attach to process, detect loaded libs, setup native breakpoints, etc.
    HRESULT Init(pid_t pid, int &error_n);
    // Shutdown interop debugging, remove all native breakpoints, detach from threads, etc.
    void Shutdown();

} // namespace InteropDebugging
#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
