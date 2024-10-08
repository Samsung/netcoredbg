// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include "debugger/interop_ptrace_helpers.h"
#include "debugger/interop_singlestep_helpers.h"
#include <cstdint>
#include <vector>

namespace netcoredbg
{
namespace InteropDebugging
{

#if DEBUGGER_UNIX_ARM

bool ARM32_DoSoftwareSingleStep(pid_t pid, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints);

#endif // DEBUGGER_UNIX_ARM

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
