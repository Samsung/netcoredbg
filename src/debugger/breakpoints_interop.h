// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef INTEROP_DEBUGGING

#include "debugger/interop_ptrace_helpers.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <functional>

namespace netcoredbg
{
namespace InteropDebugging
{

class InteropBreakpoints
{
public:

    // In case of error, return `errno`.
    int Add(pid_t pid, std::uintptr_t brkAddr, bool isThumbCode, std::function<void()> StopAllThreads);
    // In case of error, return `errno`.
    int Remove(pid_t pid, std::uintptr_t brkAddr, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    // Remove all native breakpoints at interop detach.
    void RemoveAllAtDetach(pid_t pid);
    bool IsBreakpoint(std::uintptr_t brkAddr);
    void StepOverBrk(pid_t pid, std::uintptr_t brkAddr);
    // Return `false` in case no breakpoint with this PC was found (step is not possible).
    bool StepPrevToBrk(pid_t pid, std::uintptr_t brkAddr);
    // Remove all related to unloaded library breakpoints entries in data structures.
    void UnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr);

private:

    struct MemBrk
    {
        int m_count = 0;
        word_t m_savedData = 0;
    };

    // NOTE we could recursively call `InteropBreakpoints` methods in StopAllThreads/FixAllThreads callbacks.
    std::recursive_mutex m_breakpointsMutex;
    std::unordered_map<std::uintptr_t, MemBrk> m_currentBreakpointsInMemory;
};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
