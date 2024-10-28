// Copyright (c) 2024 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_singlestep_helpers.h"
#include "debugger/interop_brk_helpers.h"

#include <errno.h>
#include <string.h>
#include "utils/logger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

bool RemoveSoftwareSingleStepBreakpoints(pid_t pid, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints)
{
    for (auto &entry : swSingleStepBreakpoints)
    {
        errno = 0;
        word_t brkData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)entry.bpAddr, nullptr);
        if (errno != 0)
        {
            char buf[1024];
            LOGE("Ptrace peekdata error: %s", ErrGetStr(errno, buf, sizeof(buf)));
            return false;
        }

        entry.restoreData = RestoredOpcode(brkData, entry.restoreData); // fix restore data in case breakpoint opcode size less than word_t

        if (async_ptrace(PTRACE_POKEDATA, pid, (void*)entry.bpAddr, (void*)entry.restoreData) == -1)
        {
            char buf[1024];
            LOGE("Ptrace pokedata error: %s\n", ErrGetStr(errno, buf, sizeof(buf)));
            return false;
        }
    }
    swSingleStepBreakpoints.clear();

    return true;
}

} // namespace InteropDebugging
} // namespace netcoredbg
