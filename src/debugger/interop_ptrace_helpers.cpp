// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_ptrace_helpers.h"

#include <sys/ptrace.h>
#if DEBUGGER_UNIX_ARM64
#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#endif

namespace netcoredbg
{
namespace InteropDebugging
{

long ptrace_GETREGS(pid_t pid, user_regs_struct &regs)
{
#if DEBUGGER_UNIX_ARM64
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov);
#else
    return ptrace(PTRACE_GETREGS, pid, nullptr, &regs);
#endif
}

long ptrace_SETREGS(pid_t pid, user_regs_struct &regs)
{
#if DEBUGGER_UNIX_ARM64
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov);
#else
    return ptrace(PTRACE_SETREGS, pid, nullptr, &regs);
#endif
}

} // namespace InteropDebugging
} // namespace netcoredbg
