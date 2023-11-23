// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/threads.h"
#include "debugger/evaluator.h"
#include "debugger/valueprint.h"
#include "utils/torelease.h"
#ifdef INTEROP_DEBUGGING
#include "debugger/interop_debugging.h"
#endif // INTEROP_DEBUGGING

namespace netcoredbg
{

ThreadId getThreadId(ICorDebugThread *pThread)
{
    DWORD threadId = 0;  // invalid value for Win32
    HRESULT res = pThread->GetID(&threadId);
    return SUCCEEDED(res) && threadId != 0 ? ThreadId{threadId} : ThreadId{};
}

void Threads::Add(const ThreadId &threadId, bool processAttached)
{
    std::unique_lock<Utility::RWLock::Writer> write_lock(m_userThreadsRWLock.writer);

    m_userThreads.emplace(threadId);
    // First added user thread during start is Main thread for sure.
    if (!processAttached && !MainThread)
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
                const std::string &memberName,
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

    if (MainThread && MainThread == userThread && threadName == "<No name>")
        return "Main Thread";

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
        threads.emplace_back(userThread, GetThreadName(pProcess, userThread), procRunning == TRUE);
    }

    return S_OK;
}

#ifdef INTEROP_DEBUGGING
// Caller should guarantee, that pProcess is not null.
HRESULT Threads::GetInteropThreadsWithState(ICorDebugProcess *pProcess, InteropDebugging::InteropDebugger *pInteropDebugger, std::vector<Thread> &threads)
{
    HRESULT Status;
    BOOL managedProcRunning = FALSE;
    IfFailRet(pProcess->IsRunning(&managedProcRunning));

    std::unordered_set<DWORD> managedThreads;
    ToRelease<ICorDebugThreadEnum> iCorThreadEnum;
    pProcess->EnumerateThreads(&iCorThreadEnum);
    ULONG fetched = 0;
    ToRelease<ICorDebugThread> iCorThread;
    while (SUCCEEDED(iCorThreadEnum->Next(1, &iCorThread, &fetched)) && fetched == 1)
    {
        DWORD tid = 0;
        if (SUCCEEDED(iCorThread->GetID(&tid)))
        {
            managedThreads.emplace(tid);
        }
        iCorThread.Free();
    }

    pInteropDebugger->WalkAllThreads([&](pid_t tid, bool isRunning)
    {
        ThreadId threadId(tid);

        if (managedThreads.find((DWORD)tid) != managedThreads.end())
            threads.emplace_back(threadId, GetThreadName(pProcess, threadId), managedProcRunning == TRUE, true);
        else
            threads.emplace_back(threadId, "<No name>", isRunning, false);
    });

    return S_OK;
}
#endif // INTEROP_DEBUGGING

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

void Threads::ResetEvaluator()
{
    m_sharedEvaluator.reset();
}

} // namespace netcoredbg
