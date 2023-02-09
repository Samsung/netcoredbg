// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <cstdint>

namespace netcoredbg
{

#ifdef DEBUGGER_UNIX_ARM
    using user_regs_struct = user_regs;
#endif

#ifdef DBG_TARGET_64BIT
    using word_t = std::uint64_t;
#else
    using word_t = std::uint32_t;
#endif

namespace InteropDebugging
{

    void async_ptrace_init();
    void async_ptrace_shutdown();
    // Note, this function call will provide `errno` of real ptrace() call.
    long async_ptrace(__ptrace_request request, pid_t pid, void *addr, void *data);

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
