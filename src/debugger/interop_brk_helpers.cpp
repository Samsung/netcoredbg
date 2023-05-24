// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_brk_helpers.h"

#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include <assert.h>
#include <string.h>
#include "debugger/waitpid.h"
#include "utils/logger.h"

namespace netcoredbg
{
namespace InteropDebugging
{

// return true if at least one register should be changed
bool NeedSetPrevBrkPC()
{
#if DEBUGGER_UNIX_AMD64
    return true; // Need step back on size of int3 (0xCC, 1 byte).
#elif DEBUGGER_UNIX_X86
    return true; // Need step back on size of int3 (0xCC, 1 byte).
#elif DEBUGGER_UNIX_ARM64
    return false; // In case of arm64 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
#elif DEBUGGER_UNIX_ARM
    return false; // In case of arm32 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
#else
#error "Unsupported platform"
#endif
}

// return true if at least one register was changed
void SetPrevBrkPC(user_regs_struct &regs)
{
#if DEBUGGER_UNIX_AMD64
    // Step back on size of int3 (0xCC, 1 byte).
    regs.rip -= 1;
#elif DEBUGGER_UNIX_X86
    // Step back on size of int3 (0xCC, 1 byte).
    regs.eip -= 1;
#elif DEBUGGER_UNIX_ARM64
    // In case of arm64 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
#elif DEBUGGER_UNIX_ARM
    // In case of arm32 breakpoint is illegal code interpreted by Linux kernel as breakpoint, no PC change need.
#else
#error "Unsupported platform"
#endif
}

// Return breakpoint address by current PC.
std::uintptr_t GetBrkAddrByPC(const user_regs_struct &regs)
{
#if DEBUGGER_UNIX_AMD64
    return std::uintptr_t(regs.rip - 1);
#elif DEBUGGER_UNIX_X86
    return std::uintptr_t(regs.eip - 1);
#elif DEBUGGER_UNIX_ARM64
    return std::uintptr_t(regs.pc);
#elif DEBUGGER_UNIX_ARM
    const static int REG_PC = 15;
    return std::uintptr_t(regs.uregs[REG_PC]);
#else
#error "Unsupported platform"
#endif
}

// Return break address by current PC.
std::uintptr_t GetBreakAddrByPC(const user_regs_struct &regs)
{
#if DEBUGGER_UNIX_AMD64
    return std::uintptr_t(regs.rip);
#elif DEBUGGER_UNIX_X86
    return std::uintptr_t(regs.eip);
#elif DEBUGGER_UNIX_ARM64
    return std::uintptr_t(regs.pc);
#elif DEBUGGER_UNIX_ARM
    const static int REG_PC = 15;
    return std::uintptr_t(regs.uregs[REG_PC]);
#else
#error "Unsupported platform"
#endif
}

#if DEBUGGER_UNIX_ARM
static bool IsThumbOpcode32Bits(word_t data)
{
    return (data & 0xe000) == 0xe000 && (data & 0x1800) != 0;
}
#endif

word_t EncodeBrkOpcode(word_t data, bool thumbCode)
{
#if DEBUGGER_UNIX_AMD64
    return ((data & ~((word_t)0xff)) | 0xcc); // 0xcc -Int3
#elif DEBUGGER_UNIX_X86
    return ((data & ~((word_t)0xff)) | 0xcc); // 0xcc -Int3
#elif DEBUGGER_UNIX_ARM64
    // `brk #0` encoded by aarch64 compillers as `0xd4200000`, also used in aarch64-tdep.c gdb source (check order! must be LE)
    // Note, arm64 have 8 bytes word, ptrace will read and write 8 bytes only by call. arm64 have 4 bytes breakpoint opcode,
    // we "clear" low 4 bytes in initial data with data & ~((word_t)0xffffffff) and "add" 4 bytes of arm64 breakpoint opcode.
    return ((data & ~((word_t)0xffffffff)) | 0xd4200000);
#elif DEBUGGER_UNIX_ARM
    // TODO investigate `bkpt #0` work on arm32
    // Current implementation:
    // The point is - breakpoint in arm32 (arm, thumb and thumb2) is just illegal instruction, that kernel (Linux kernel in our case) interpret as breakpoint
    // and sent proper signal (since we ptrace process). This mean, "real" breakpoints opcodes could be found in Linux kernel:
    //     https://github.com/torvalds/linux/blob/8ca09d5fa3549d142c2080a72a4c70ce389163cd/arch/arm/kernel/ptrace.c#L212-L234
    // Usage example:
    //     https://github.com/qemu/qemu/blob/9832009d9dd2386664c15cc70f6e6bfe062be8bd/linux-user/arm/cpu_loop.c#L241-L257
    if (!thumbCode)
        return 0x07f001f0; // arm code breakpoint

    if (IsThumbOpcode32Bits(data))
        return 0xa000f7f0; // 4 bytes thumb breakpoint
    else
        return (data & ~((word_t)0xffff)) | 0xde01; // 2 bytes thumb breakpoint
#else
#error "Unsupported platform"
#endif
}

word_t RestoredOpcode(word_t dataWithBrk, word_t restoreData)
{
#if DEBUGGER_UNIX_AMD64
    return (dataWithBrk & ~((word_t)0xff)) | (restoreData & 0xff);
#elif DEBUGGER_UNIX_X86
    return (dataWithBrk & ~((word_t)0xff)) | (restoreData & 0xff);
#elif DEBUGGER_UNIX_ARM64
    return (dataWithBrk & ~((word_t)0xffffffff)) | (restoreData & 0xffffffff);
#elif DEBUGGER_UNIX_ARM
    if (dataWithBrk == 0x07f001f0 || dataWithBrk == 0xa000f7f0) // arm or 4 bytes thumb breakpoint
        return restoreData;

    // 2 bytes thumb breakpoint
    return (dataWithBrk & ~((word_t)0xffff)) | (restoreData & 0xffff);
#else
#error "Unsupported platform"
#endif
}

bool StepOverBrk(pid_t pid, std::uintptr_t addr, word_t restoreData, std::function<bool(pid_t, std::uintptr_t)> SingleStepOnBrk)
{
    // We have 2 cases here (at breakpoint stop):
    //   * x86/amd64 already changed PC (executed 0xCC code), so, SetPrevBrkPC() call will change PC in our stored registers
    //     and return `true`, after that we need set this registers into thread by ptrace(PTRACE_SETREGSET);
    //   * arm32/arm64 don't move PC at breakpoint, so, SetPrevBrkPC() will return `false`, since PC was not changed
    //     and we don't need call ptrace(PTRACE_SETREGSET) in order to set changed registers (PC).
    if (NeedSetPrevBrkPC())
    {
        user_regs_struct regs;
        iovec iov;
        iov.iov_base = &regs;
        iov.iov_len = sizeof(user_regs_struct);
        if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
        {
            LOGE("Ptrace getregset error: %s\n", strerror(errno));
            return false;
        }

        SetPrevBrkPC(regs);

        if (async_ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
        {
            LOGE("Ptrace setregset error: %s\n", strerror(errno));
            return false;
        }
    }

    errno = 0;
    word_t brkData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)addr, nullptr);
    if (errno != 0)
    {
        LOGE("Ptrace peekdata error: %s", strerror(errno));
        return false;
    }

    restoreData = RestoredOpcode(brkData, restoreData);

    // restore data
    if (async_ptrace(PTRACE_POKEDATA, pid, (void*)addr, (void*)restoreData) == -1)
    {
        LOGE("Ptrace pokedata error: %s\n", strerror(errno));
        return false;
    }

    if (!SingleStepOnBrk(pid, addr))
        return false;

    // setup bp again
    if (async_ptrace(PTRACE_POKEDATA, pid, (void*)addr, (void*)brkData) == -1)
    {
        LOGE("Ptrace pokedata error: %s\n", strerror(errno));
        return false;
    }

    return true;
}

} // namespace InteropDebugging
} // namespace netcoredbg
