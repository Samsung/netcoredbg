// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/threads.h"
#include "torelease.h"

namespace netcoredbg
{

ThreadId getThreadId(ICorDebugThread *pThread)
{
    DWORD threadId = 0;  // invalid value for Win32
    HRESULT res = pThread->GetID(&threadId);
    return SUCCEEDED(res) && threadId != 0 ? ThreadId{threadId} : ThreadId{};
}

void Threads::Add(const ThreadId &threadId)
{
    m_userThreadsMutex.lock();
    m_userThreads.emplace(threadId);
    m_userThreadsMutex.unlock();
}

void Threads::Remove(const ThreadId &threadId)
{
    std::lock_guard<std::mutex> lock(m_userThreadsMutex);

    auto it = m_userThreads.find(threadId);
    if (it == m_userThreads.end())
        return;

    m_userThreads.erase(it);
}

HRESULT Threads::GetThreadsWithState(ICorDebugProcess *pProcess, std::vector<Thread> &threads)
{
    if (!pProcess)
        return E_FAIL;

    std::lock_guard<std::mutex> lock(m_userThreadsMutex);

    HRESULT Status;
    BOOL procRunning = FALSE;
    IfFailRet(pProcess->IsRunning(&procRunning));

    const std::string threadName = "<No name>";
    for (auto &userThread : m_userThreads)
    {
        // ICorDebugThread::GetUserState not available for running thread.
        threads.emplace_back(userThread, threadName, procRunning);
    }

    return S_OK;
}

} // namespace netcoredbg
