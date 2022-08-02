// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/threads.h"
#include "debugger/evaluator.h"
#include "debugger/valueprint.h"
#include "utils/torelease.h"

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
    std::unique_lock<Utility::RWLock::Writer> write_lock(m_userThreadsRWLock.writer);

    m_userThreads.emplace(threadId);
    // First added user thread is Main thread for sure.
    if (!MainThread)
        MainThread = threadId;
}

void Threads::Remove(const ThreadId &threadId)
{
    std::unique_lock<Utility::RWLock::Writer> write_lock(m_userThreadsRWLock.writer);

    auto it = m_userThreads.find(threadId);
    if (it == m_userThreads.end())
        return;

    m_userThreads.erase(it);
}

std::string Threads::GetThreadName(ICorDebugProcess *pProcess, const ThreadId &userThread)
{
    if (MainThread == userThread)
        return "Main Thread";

    std::string threadName = "<No name>";

    if (m_sharedEvaluator)
    {
        ToRelease<ICorDebugThread> pThread;
        ToRelease<ICorDebugValue> iCorThreadObject;
        if (SUCCEEDED(pProcess->GetThread(int(userThread), &pThread)) &&
            SUCCEEDED(pThread->GetObject(&iCorThreadObject)))
        {
            HRESULT Status;
            m_sharedEvaluator->WalkMembers(iCorThreadObject, nullptr, FrameLevel{0}, false, [&](
                ICorDebugType *,
                bool,
                const std::string  &memberName,
                Evaluator::GetValueCallback getValue,
                Evaluator::SetterData*)
            {
                // Note, only field here (not `Name` property), since we can't guarantee code execution (call property's getter),
                // this thread can be in not consistent state for evaluation or thread could break in optimized code.
                if (memberName != "_name")
                    return S_OK;

                ToRelease<ICorDebugValue> iCorResultValue;
                IfFailRet(getValue(&iCorResultValue, defaultEvalFlags));

                BOOL isNull = TRUE;
                ToRelease<ICorDebugValue> pValue;
                IfFailRet(DereferenceAndUnboxValue(iCorResultValue, &pValue, &isNull));
                if (!isNull)
                    IfFailRet(PrintStringValue(pValue, threadName));

                return E_ABORT; // Fast exit from cycle.
            });
        }
    }

    return threadName;
}

// Caller should guarantee, that pProcess is not null.
HRESULT Threads::GetThreadsWithState(ICorDebugProcess *pProcess, std::vector<Thread> &threads)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(m_userThreadsRWLock.reader);

    HRESULT Status;
    BOOL procRunning = FALSE;
    IfFailRet(pProcess->IsRunning(&procRunning));

    threads.reserve(m_userThreads.size());
    for (auto &userThread : m_userThreads)
    {
        // ICorDebugThread::GetUserState not available for running thread.
        threads.emplace_back(userThread, GetThreadName(pProcess, userThread), procRunning);
    }

    return S_OK;
}

HRESULT Threads::GetThreadIds(std::vector<ThreadId> &threads)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(m_userThreadsRWLock.reader);

    threads.reserve(m_userThreads.size());
    for (auto &userThread : m_userThreads)
    {
        threads.emplace_back(userThread);
    }
    return S_OK;
}

void Threads::SetEvaluator(std::shared_ptr<Evaluator> &sharedEvaluator)
{
    m_sharedEvaluator = sharedEvaluator;
}

} // namespace netcoredbg
