// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include "debugger/interop_ptrace_helpers.h"
#include <functional>

namespace netcoredbg
{
namespace InteropDebugging
{

    bool NeedSetPrevBrkPC(); // return true if at least one register should be changed
    void SetPrevBrkPC(user_regs_struct &regs);
    std::uintptr_t GetBrkAddrByPC(const user_regs_struct &regs);
    std::uintptr_t GetBreakAddrByPC(const user_regs_struct &regs);
    word_t EncodeBrkOpcode(word_t data, bool thumbCode);
    word_t RestoredOpcode(word_t dataWithBrk, word_t restoreData);
    bool StepOverBrk(pid_t pid, std::uintptr_t addr, word_t restoreData, std::function<bool(pid_t, std::uintptr_t)> SingleStepOnBrk);

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
