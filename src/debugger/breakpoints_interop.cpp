// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_interop.h"
#include "debugger/interop_brk_helpers.h"
#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include <assert.h>
#include <string.h>
#include "utils/logger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

int InteropBreakpoints::Add(pid_t pid, std::uintptr_t brkAddr, bool isThumbCode, std::function<void()> StopAllThreads)
{
    m_breakpointsMutex.lock();

    word_t savedData = 0;
    word_t dataWithBrk = 0;

    auto find = m_currentBreakpointsInMemory.find(brkAddr);
    if (find == m_currentBreakpointsInMemory.end())
    {
        StopAllThreads();
        errno = 0;  // Since the value returned by a successful PTRACE_PEEK* request may be -1, the caller must clear errno before the call,
                    // and then check it afterward to determine whether or not an error occurred.
        savedData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)brkAddr, nullptr);
        if (errno != 0)
        {
            int err_code = errno;
            LOGE("Ptrace peekdata error: %s", strerror(err_code));
            return err_code;
        }
        dataWithBrk = EncodeBrkOpcode(savedData, isThumbCode);
        if (async_ptrace(PTRACE_POKEDATA, pid, (void*)brkAddr, (void*)dataWithBrk) == -1)
        {
            int err_code = errno;
            LOGE("Ptrace pokedata error: %s", strerror(err_code));
            return err_code;
        }

        m_currentBreakpointsInMemory[brkAddr].m_savedData = savedData;
    }

    m_currentBreakpointsInMemory[brkAddr].m_count++;

    m_breakpointsMutex.unlock();
    return 0;
}

int InteropBreakpoints::Remove(pid_t pid, std::uintptr_t brkAddr, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    std::lock_guard<std::recursive_mutex> lock(m_breakpointsMutex);

    auto find = m_currentBreakpointsInMemory.find(brkAddr);
    if (find == m_currentBreakpointsInMemory.end())
        return ENOENT;

    assert(find->second.m_count > 0);

    find->second.m_count--;
    if (find->second.m_count == 0)
    {
        StopAllThreads();
        FixAllThreads(find->first);

        errno = 0;
        word_t brkData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)find->first, nullptr);
        if (errno != 0)
        {
            int err_code = errno;
            LOGE("Ptrace peekdata error: %s", strerror(err_code));
            return err_code;
        }
        word_t restoredData = RestoredOpcode(brkData, find->second.m_savedData);

        if (async_ptrace(PTRACE_POKEDATA, pid, (void*)find->first, (void*)restoredData) == -1)
        {
            int err_code = errno;
            LOGW("Ptrace pokedata error: %s\n", strerror(err_code));
            return err_code;
        }
        m_currentBreakpointsInMemory.erase(find);
    }
    return 0;
}

// Must be called only in case all threads stopped and fixed (see InteropDebugger::StopAndDetach()).
void InteropBreakpoints::RemoveAllAtDetach(pid_t pid)
{
    m_breakpointsMutex.lock();

    if (pid != 0) // In case we already don't have process, no need real remove from memory.
    {
        for (auto entry : m_currentBreakpointsInMemory)
        {
            errno = 0;
            word_t brkData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)entry.first, nullptr);
            if (errno != 0)
            {
                LOGE("Ptrace peekdata error: %s", strerror(errno));
            }
            word_t restoredData = RestoredOpcode(brkData, entry.second.m_savedData);

            if (async_ptrace(PTRACE_POKEDATA, pid, (void*)entry.first, (void*)restoredData) == -1)
            {
                LOGW("Ptrace pokedata error: %s\n", strerror(errno));
            }
        }
    }

    m_currentBreakpointsInMemory.clear();

    m_breakpointsMutex.unlock();
}

bool InteropBreakpoints::IsBreakpoint(std::uintptr_t brkAddr)
{
    std::lock_guard<std::recursive_mutex> lock(m_breakpointsMutex);

    return m_currentBreakpointsInMemory.find(brkAddr) != m_currentBreakpointsInMemory.end();
}

void InteropBreakpoints::StepOverBrk(pid_t pid, std::uintptr_t brkAddr)
{
    std::lock_guard<std::recursive_mutex> lock(m_breakpointsMutex);

    auto find = m_currentBreakpointsInMemory.find(brkAddr);
    if (find == m_currentBreakpointsInMemory.end())
        return;

    InteropDebugging::StepOverBrk(pid, brkAddr, find->second.m_savedData);
}

bool InteropBreakpoints::StepPrevToBrk(pid_t pid, std::uintptr_t brkAddr)
{
    std::lock_guard<std::recursive_mutex> lock(m_breakpointsMutex);

    auto find = m_currentBreakpointsInMemory.find(brkAddr);
    if (find == m_currentBreakpointsInMemory.end())
        return false;

    if (!NeedSetPrevBrkPC())
        return true;

    user_regs_struct regs;
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(user_regs_struct);
    if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
        LOGW("Ptrace getregset error: %s\n", strerror(errno));

    SetPrevBrkPC(regs);

    if (async_ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
        LOGW("Ptrace setregset error: %s\n", strerror(errno));

    return true;
}

void InteropBreakpoints::UnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr)
{
    m_breakpointsMutex.lock();

    for (auto it = m_currentBreakpointsInMemory.begin(); it != m_currentBreakpointsInMemory.end();)
    {
        if (it->first >= startAddr && it->first < endAddr)
            it = m_currentBreakpointsInMemory.erase(it);
        else
            ++it;
    }

    m_breakpointsMutex.unlock();
}

} // namespace InteropDebugging
} // namespace netcoredbg
