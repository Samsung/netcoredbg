// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"

#include <sys/types.h>
#include <sys/user.h>
#include <cstdint>

namespace netcoredbg
{

#ifdef INTEROP_DEBUGGING
namespace InteropDebugging
{

#ifdef DEBUGGER_UNIX_ARM
    using user_regs_struct = user_regs;
#endif

#ifdef DBG_TARGET_64BIT
    using word_t = std::uint64_t;
#else
    using word_t = std::uint32_t;
#endif

    long ptrace_GETREGS(pid_t pid, user_regs_struct &regs);
    long ptrace_SETREGS(pid_t pid, user_regs_struct &regs);

} // namespace InteropDebugging
#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
