// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"

#include "debugger/interop_ptrace_helpers.h"

namespace netcoredbg
{

#ifdef INTEROP_DEBUGGING
namespace InteropDebugging
{

    bool IsEqualToBrkPC(const user_regs_struct &regs, std::uintptr_t addr);
    bool SetPrevBrkPC(user_regs_struct &regs); // return true if at least one register was changed
    word_t EncodeBrkOpcode(word_t data);
    void StepOverBrk(pid_t pid, user_regs_struct &regs, std::uintptr_t addr, word_t restoreData, word_t brkData);

} // namespace InteropDebugging
#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
