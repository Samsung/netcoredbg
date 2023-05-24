// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_unwind.h"
#include "debugger/interop_ptrace_helpers.h"

#include <libunwind-ptrace.h> // _UPT_find_proc_info()
#include <sys/mman.h>
#include <array>
#include <cstring>
#include <errno.h>
#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include "utils/logger.h"
#include "metadata/interop_libraries.h"


namespace netcoredbg
{
namespace InteropDebugging
{

struct elf_image_t
{
    void *image; // pointer to mmap'd image
    size_t size; // (file-) size of the image
};

struct elf_dyn_info_t
{
    elf_image_t ei;
    unw_dyn_info_t di_cache;
    unw_dyn_info_t di_debug; // additional table info for .debug_frame
#if UNW_TARGET_ARM
    unw_dyn_info_t di_arm; // additional table info for .ARM.exidx
#endif
};

struct UPT_info
{
    struct
    {
        pid_t pid; // the process-id of the child we're unwinding
        elf_dyn_info_t edi;
    } libunwind_UPT_info;
    const std::array<unw_word_t, UNW_REG_LAST + 1> *contextRegs;
};

static inline void invalidate_edi(elf_dyn_info_t *edi)
{
    if (edi->ei.image)
    {
        munmap(edi->ei.image, edi->ei.size);
    }
    memset(edi, 0, sizeof(*edi));
    edi->di_cache.format = -1;
    edi->di_debug.format = -1;
#if UNW_TARGET_ARM
    edi->di_arm.format = -1;
#endif
}

// ptrace related registers data (see <sys/user.h>).
static std::array<int, UNW_REG_LAST + 1> InitPtraceRegOffset()
{
    std::array<int, UNW_REG_LAST + 1> res;

#if defined(UNW_TARGET_X86)
    res[UNW_X86_EAX]       = 0x18;
    res[UNW_X86_EBX]       = 0x00;
    res[UNW_X86_ECX]       = 0x04;
    res[UNW_X86_EDX]       = 0x08;
    res[UNW_X86_ESI]       = 0x0c;
    res[UNW_X86_EDI]       = 0x10;
    res[UNW_X86_EBP]       = 0x14;
    res[UNW_X86_EIP]       = 0x30;
    res[UNW_X86_ESP]       = 0x3c;
#elif defined(UNW_TARGET_X86_64)
    res[UNW_X86_64_RAX]    = 0x50;
    res[UNW_X86_64_RDX]    = 0x60;
    res[UNW_X86_64_RCX]    = 0x58;
    res[UNW_X86_64_RBX]    = 0x28;
    res[UNW_X86_64_RSI]    = 0x68;
    res[UNW_X86_64_RDI]    = 0x70;
    res[UNW_X86_64_RBP]    = 0x20;
    res[UNW_X86_64_RSP]    = 0x98;
    res[UNW_X86_64_R8]     = 0x48;
    res[UNW_X86_64_R9]     = 0x40;
    res[UNW_X86_64_R10]    = 0x38;
    res[UNW_X86_64_R11]    = 0x30;
    res[UNW_X86_64_R12]    = 0x18;
    res[UNW_X86_64_R13]    = 0x10;
    res[UNW_X86_64_R14]    = 0x08;
    res[UNW_X86_64_R15]    = 0x00;
    res[UNW_X86_64_RIP]    = 0x80;
#elif defined(UNW_TARGET_ARM)
    res[UNW_ARM_R0]        = 0x00;
    res[UNW_ARM_R1]        = 0x04;
    res[UNW_ARM_R2]        = 0x08;
    res[UNW_ARM_R3]        = 0x0c;
    res[UNW_ARM_R4]        = 0x10;
    res[UNW_ARM_R5]        = 0x14;
    res[UNW_ARM_R6]        = 0x18;
    res[UNW_ARM_R7]        = 0x1c;
    res[UNW_ARM_R8]        = 0x20;
    res[UNW_ARM_R9]        = 0x24;
    res[UNW_ARM_R10]       = 0x28;
    res[UNW_ARM_R11]       = 0x2c;
    res[UNW_ARM_R12]       = 0x30;
    res[UNW_ARM_R13]       = 0x34;
    res[UNW_ARM_R14]       = 0x38;
    res[UNW_ARM_R15]       = 0x3c;
#elif defined(UNW_TARGET_AARCH64)
    res[UNW_AARCH64_X0]       = 0x00;
    res[UNW_AARCH64_X1]       = 0x08;
    res[UNW_AARCH64_X2]       = 0x10;
    res[UNW_AARCH64_X3]       = 0x18;
    res[UNW_AARCH64_X4]       = 0x20;
    res[UNW_AARCH64_X5]       = 0x28;
    res[UNW_AARCH64_X6]       = 0x30;
    res[UNW_AARCH64_X7]       = 0x38;
    res[UNW_AARCH64_X8]       = 0x40;
    res[UNW_AARCH64_X9]       = 0x48;
    res[UNW_AARCH64_X10]      = 0x50;
    res[UNW_AARCH64_X11]      = 0x58;
    res[UNW_AARCH64_X12]      = 0x60;
    res[UNW_AARCH64_X13]      = 0x68;
    res[UNW_AARCH64_X14]      = 0x70;
    res[UNW_AARCH64_X15]      = 0x78;
    res[UNW_AARCH64_X16]      = 0x80;
    res[UNW_AARCH64_X17]      = 0x88;
    res[UNW_AARCH64_X18]      = 0x90;
    res[UNW_AARCH64_X19]      = 0x98;
    res[UNW_AARCH64_X20]      = 0xa0;
    res[UNW_AARCH64_X21]      = 0xa8;
    res[UNW_AARCH64_X22]      = 0xb0;
    res[UNW_AARCH64_X23]      = 0xb8;
    res[UNW_AARCH64_X24]      = 0xc0;
    res[UNW_AARCH64_X25]      = 0xc8;
    res[UNW_AARCH64_X26]      = 0xd0;
    res[UNW_AARCH64_X27]      = 0xd8;
    res[UNW_AARCH64_X28]      = 0xe0;
    res[UNW_AARCH64_X29]      = 0xe8; // FP
    res[UNW_AARCH64_X30]      = 0xf0; // LR
    res[UNW_AARCH64_SP]       = 0xf8;
    res[UNW_AARCH64_PC]       = 0x100;
    res[UNW_AARCH64_PSTATE]   = 0x108;
#else
#error "Unsupported platform"
#endif

    return res;
}
const std::array<int, UNW_REG_LAST + 1> g_ptraceRegOffset(InitPtraceRegOffset());




static void *UnwindContextCreate(pid_t pid, const std::array<unw_word_t, UNW_REG_LAST + 1> *contextRegs)
{
    UPT_info *ui = (UPT_info*)malloc(sizeof(UPT_info));
    if (!ui)
        return nullptr;

    memset(ui, 0, sizeof(*ui));
    ui->libunwind_UPT_info.pid = pid;
    ui->contextRegs = contextRegs;
    ui->libunwind_UPT_info.edi.di_cache.format = -1;
    ui->libunwind_UPT_info.edi.di_debug.format = -1;
    return ui;
}

static void UnwindContextDestroy(void *ptr)
{
    UPT_info *ui = (UPT_info*)ptr;
    invalidate_edi(&ui->libunwind_UPT_info.edi);
    free(ptr);
}

static int FindProcInfo(unw_addr_space_t as, unw_word_t ip, unw_proc_info_t *pi, int need_unwind_info, void *arg)
{
    UPT_info *ui = (UPT_info*)arg;
    return _UPT_find_proc_info(as, ip, pi, need_unwind_info, &ui->libunwind_UPT_info);
}

static void PutUnwindInfo(unw_addr_space_t as, unw_proc_info_t *pi, void *arg)
{
    if (!pi->unwind_info)
        return;
    free (pi->unwind_info);
    pi->unwind_info = NULL;
}

static int GetDynInfoListAddr(unw_addr_space_t, unw_word_t *, void *)
{
    // TODO there is currently no way to locate the dyn-info list by a remote unwinder. On ia64, this is done via a special
    //      unwind-table entry. Perhaps something similar can be done with DWARF2 unwind info. 
    return -UNW_ENOINFO;
}

static int AccessMem(unw_addr_space_t as, unw_word_t addr, unw_word_t *val, int write, void *arg)
{
    if (write)
        return -UNW_EINVAL;

    UPT_info *ui = (UPT_info*)arg;
    if (!ui)
        return -UNW_EINVAL;

    errno = 0;
    *val = async_ptrace(PTRACE_PEEKDATA, ui->libunwind_UPT_info.pid, (void*)addr, 0);

    return errno ? -UNW_EINVAL : 0;
}

static int AccessReg(unw_addr_space_t as, unw_regnum_t reg, unw_word_t *val, int write, void *arg)
{
    if (write)
        return -UNW_EINVAL;

    UPT_info *ui = (UPT_info*)arg;

    if (ui->contextRegs)
    {
        if ((unsigned)reg >= ui->contextRegs->size())
            return -UNW_EBADREG;

        *val = (*(ui->contextRegs))[reg];
        return 0;
    }

    if ((unsigned)reg >= g_ptraceRegOffset.size())
        return -UNW_EBADREG;

    user_regs_struct regs;
    iovec loc;
    loc.iov_base = &regs;
    loc.iov_len = sizeof(regs);

    if (async_ptrace(PTRACE_GETREGSET, ui->libunwind_UPT_info.pid, (void*)NT_PRSTATUS, &loc) == -1)
        return -UNW_EBADREG;

    char *r = (char*)&regs + g_ptraceRegOffset[reg];
    memcpy(val, r, sizeof(unw_word_t));

    return 0;
}

static int AccessFpreg(unw_addr_space_t as, unw_regnum_t reg, unw_fpreg_t *val, int write, void *arg)
{
    // We don't need this for sure.
    return -UNW_EINVAL;
}

static int GetProcName(unw_addr_space_t, unw_word_t, char *, size_t, unw_word_t *, void *)
{
    // We don't need this for sure.
    return -UNW_EINVAL;
}

static int ResumeExecution(unw_addr_space_t, unw_cursor_t *, void *)
{
    // We don't need this for sure.
    return -UNW_EINVAL;
}

void ThreadStackUnwind(pid_t pid, std::array<unw_word_t, UNW_REG_LAST + 1> *contextRegs, std::function<bool(std::uintptr_t)> threadStackUnwindCallback)
{
    // TODO ? setup for arm32 env UNW_ARM_UNWIND_METHOD with value UNW_ARM_METHOD_FRAME (looks like all unwinding good by default, no unwind method changes needed)

    // TODO ? use global cache for libunwinde (increase unwinding speed in exchange of memory usage)
    //  - unw_create_addr_space() at "unwind init"
    //  - unw_set_caching_policy() with UNW_CACHE_GLOBAL https://www.nongnu.org/libunwind/man/unw_set_caching_policy(3).html
    //  - unw_flush_cache() in case some lib unload or at "unwind shutdown" https://www.nongnu.org/libunwind/man/unw_flush_cache(3).html

    static unw_accessors_t accessors =
    {
        .find_proc_info             = FindProcInfo,
        .put_unwind_info            = PutUnwindInfo,
        .get_dyn_info_list_addr     = GetDynInfoListAddr,
        .access_mem                 = AccessMem,
        .access_reg                 = AccessReg,
        .access_fpreg               = AccessFpreg,
        .resume                     = ResumeExecution,
        .get_proc_name              = GetProcName
    };

    unw_addr_space_t addrSpace = unw_create_addr_space(&accessors, 0);
    void *unwind_context = UnwindContextCreate(pid, contextRegs);
    unw_cursor_t unwind_cursor;
    if (unw_init_remote(&unwind_cursor, addrSpace, unwind_context) < 0)
        LOGE("ERROR: cannot initialize cursor for remote unwinding");
    else
    {
#if defined(UNW_TARGET_AARCH64)
        unw_word_t prev_pc = 0;
#endif
        do
        {
            unw_word_t pc;
            if (unw_get_reg(&unwind_cursor, UNW_REG_IP, &pc) < 0)
            {
                LOGE("ERROR: cannot read program counter");
                break;
            }

            if (pc == 0)
                break;

#if defined(UNW_TARGET_AARCH64)
            if (prev_pc == pc)
                break;
            else
                prev_pc = pc;
#endif

            if (!threadStackUnwindCallback(pc))
                break;
        }
        while (unw_step(&unwind_cursor) > 0);
    }
    UnwindContextDestroy(unwind_context);
    unw_destroy_addr_space(addrSpace);
}


} // namespace InteropDebugging
} // namespace netcoredbg
