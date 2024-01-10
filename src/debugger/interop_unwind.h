// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include <sys/types.h>
#include <cstdint>
#include <functional>
#include <libunwind.h>

namespace netcoredbg
{
namespace InteropDebugging
{

void ThreadStackUnwind(pid_t pid, std::array<unw_word_t, UNW_REG_LAST + 1> *contextRegs, std::function<bool(std::uintptr_t)> threadStackUnwindCallback);

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
