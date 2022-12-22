// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_brk_helpers.h"

#include <sys/ptrace.h>
#include "debugger/waitpid.h"
#include "utils/logger.h"

namespace netcoredbg
{
namespace InteropDebugging
{

bool IsEqualToBrkPC(const user_regs_struct &regs, std::uintptr_t addr)
{
#if DEBUGGER_UNIX_AMD64
    assert(regs.rip > 0);
    // "real" break address is PC minus size of int3 (0xCC, 1 byte).
    return std::uintptr_t(regs.rip - 1) == addr;
#elif DEBUGGER_UNIX_X86
    assert(regs.eip > 0);
    // "real" break address is PC minus size of int3 (0xCC, 1 byte).
    return std::uintptr_t(regs.eip - 1) == addr;
#elif DEBUGGER_UNIX_ARM64
    // In case of arm64 breakpoint is illigale code, that was not executed and PC point on real break address.
    return regs.pc == addr;
#elif DEBUGGER_UNIX_ARM
    // In case of arm32 breakpoint is illigale code, that was not executed and PC point on real break address.
    const static int REG_PC = 15;
    return regs.uregs[REG_PC] == addr;
#else
    std::abort();
#endif
}

// return true if at least one register was changed
bool SetPrevBrkPC(user_regs_struct &regs)
{
#if DEBUGGER_UNIX_AMD64
    // Step back on size of int3 (0xCC, 1 byte).
    regs.rip -= 1;
    return true;
#elif DEBUGGER_UNIX_X86
    // Step back on size of int3 (0xCC, 1 byte).
    regs.eip -= 1;
    return true;
#elif DEBUGGER_UNIX_ARM64
    // In case of arm64 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
    return false;
#elif DEBUGGER_UNIX_ARM
    // In case of arm32 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
    return false;
#else
    std::abort();
#endif
}

word_t EncodeBrkOpcode(word_t data)
{
#if DEBUGGER_UNIX_AMD64
    return ((data & ~0xff) | 0xcc); // 0xcc -Int3
#elif DEBUGGER_UNIX_X86
    return ((data & ~0xff) | 0xcc); // 0xcc -Int3
#elif DEBUGGER_UNIX_ARM64
    return 0xd4200000; // `brk #0` encoded by aarch64 compillers as `0xd4200000`, also used in aarch64-tdep.c gdb source    (check order! must be LE, Linux kernel count on this)
#elif DEBUGGER_UNIX_ARM
/*
From Linux kernel:
#define AARCH32_BREAK_ARM	0x07f001f0
#define AARCH32_BREAK_THUMB	0xde01
#define AARCH32_BREAK_THUMB2_LO	0xf7f0
#define AARCH32_BREAK_THUMB2_HI	0xa000
*/
    return 0x07f001f0; // arm     TODO thumb (0xde01 - brk opcode) and thumb2    (check order! all arm/thumb/thumb2 must be LE, Linux kernel count on this)
#else
    std::abort();
#endif
}

void StepOverBrk(pid_t pid, user_regs_struct &regs, std::uintptr_t addr, word_t restoreData, word_t brkData)
{
    // We have 2 cases here (at breakpoint stop):
    //   * x86/amd64 already changed PC (executed 0xCC code), so, SetPrevBrkPC() call will change PC in our stored registers
    //     and return `true`, after that we need set this registers into thread by ptrace_SETREGS();
    //   * arm32/arm64 don't move PC at breakpoint, so, SetPrevBrkPC() will return `false`, since PC was not changed
    //     and we don't need call ptrace_SETREGS() in order to set changed registers (PC).
    if (SetPrevBrkPC(regs) && ptrace_SETREGS(pid, regs) == -1)
        LOGW("Ptrace setregs error: %s\n", strerror(errno));

    // restore data
    if (ptrace(PTRACE_POKEDATA, pid, addr, restoreData) == -1)
        LOGW("Ptrace pokedata error: %s\n", strerror(errno));

    // single step
    if (ptrace(PTRACE_SINGLESTEP, pid, nullptr, nullptr) == -1)
        LOGW("Ptrace singlestep error: %s\n", strerror(errno));

    int wait_status;
    GetWaitpid()(pid, &wait_status, __WALL);
    // TODO check that we get SIGTRAP + TRAP_TRACE here before continue

    // setup bp again
    if (ptrace(PTRACE_POKEDATA, pid, addr, brkData) == -1)
        LOGW("Ptrace pokedata error: %s\n", strerror(errno));
}

} // namespace InteropDebugging
} // namespace netcoredbg
