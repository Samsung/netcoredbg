// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifdef _MSC_VER
#include <wtypes.h>
#endif

#include "debugger/callbacksqueue.h"
#include "debugger/threads.h"
#include "debugger/evalwaiter.h"
#include "debugger/breakpoints.h"
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "interfaces/iprotocol.h"

#include <algorithm>

namespace netcoredbg
{

bool CallbacksQueue::CallbacksWorkerBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    // S_FALSE or error - continue callback.
    // S_OK - this is internal Hot Reload breakpoint, ignore this callback call.
    if (S_OK == m_debugger.m_sharedBreakpoints->CheckApplicationReload(pThread, pBreakpoint))
        return false;

    // S_FALSE - not error and steppers not affect on callback
    if (S_FALSE != m_debugger.m_uniqueSteppers->ManagedCallbackBreakpoint(pAppDomain, pThread))
        return false;

    bool atEntry = false;
    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopBreakpoint, threadId);
    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_sharedBreakpoints->ManagedCallbackBreakpoint(pThread, pBreakpoint, event.breakpoint, atEntry))
        return false;

    // Disable all steppers if we stop at breakpoint during step.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(pAppDomain);

    if (atEntry)
        event.reason = StopEntry;

    ToRelease<ICorDebugFrame> pFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
        m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), event.frame);

    m_debugger.SetLastStoppedThread(pThread);
    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return true;
}

bool CallbacksQueue::CallbacksWorkerStepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, CorDebugStepReason reason)
{
    m_debugger.m_sharedBreakpoints->CheckApplicationReload(pThread);

    // S_FALSE - not error and steppers not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_uniqueSteppers->ManagedCallbackStepComplete(pThread, reason))
        return false;

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> iCorFrame;
    ThreadId threadId(getThreadId(pThread));
    if (SUCCEEDED(pThread->GetActiveFrame(&iCorFrame)) && iCorFrame != nullptr)
        m_debugger.GetFrameLocation(iCorFrame, threadId, FrameLevel(0), stackFrame);

    StoppedEvent event(StopStep, threadId);
    event.frame = stackFrame;

    m_debugger.SetLastStoppedThread(pThread);
    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return true;
}

bool CallbacksQueue::CallbacksWorkerBreak(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    m_debugger.m_sharedBreakpoints->CheckApplicationReload(pThread);

    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_sharedBreakpoints->ManagedCallbackBreak(pThread, m_debugger.GetLastStoppedThreadId()))
        return false;

    // Disable all steppers if we stop at break during step.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(pAppDomain);

    m_debugger.SetLastStoppedThread(pThread);
    ThreadId threadId(getThreadId(pThread));
    StackFrame stackFrame;

    ToRelease<ICorDebugFrame> iCorFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&iCorFrame)) && iCorFrame != nullptr)
        m_debugger.GetFrameLocation(iCorFrame, threadId, FrameLevel(0), stackFrame);

    StoppedEvent event(StopPause, threadId);
    event.frame = stackFrame;
    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return true;
}

bool CallbacksQueue::CallbacksWorkerException(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ExceptionCallbackType eventType, const std::string &excModule)
{
    m_debugger.m_sharedBreakpoints->CheckApplicationReload(pThread);

    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopException, threadId);

    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_sharedBreakpoints->ManagedCallbackException(pThread, eventType, excModule, event))
        return false;

    ToRelease<ICorDebugFrame> pActiveFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pActiveFrame)) && pActiveFrame != nullptr)
        m_debugger.GetFrameLocation(pActiveFrame, threadId, FrameLevel(0), event.frame);

    // Disable all steppers if we stop during step.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(pAppDomain);

    m_debugger.SetLastStoppedThread(pThread);

    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return true;
}

bool CallbacksQueue::CallbacksWorkerCreateProcess()
{
    m_debugger.NotifyProcessCreated();
    return false;
}

void CallbacksQueue::CallbacksWorker()
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    while (true)
    {
        while (m_callbacksQueue.empty() || m_stopEventInProcess)
        {
            // Note, during m_callbacksCV.wait() (waiting for notify_one call with entry added into queue),
            // m_callbacksMutex will be unlocked (see std::condition_variable for more info).
            m_callbacksCV.wait(lock);
        }

        auto &c = m_callbacksQueue.front();

        switch (c.Call)
        {
        case CallbackQueueCall::Breakpoint:
            m_stopEventInProcess = CallbacksWorkerBreakpoint(c.iCorAppDomain, c.iCorThread, c.iCorBreakpoint);
            break;
        case CallbackQueueCall::StepComplete:
            m_stopEventInProcess = CallbacksWorkerStepComplete(c.iCorAppDomain, c.iCorThread, c.Reason);
            break;
        case CallbackQueueCall::Break:
            m_stopEventInProcess = CallbacksWorkerBreak(c.iCorAppDomain, c.iCorThread);
            break;
        case CallbackQueueCall::Exception:
            m_stopEventInProcess = CallbacksWorkerException(c.iCorAppDomain, c.iCorThread, c.EventType, c.ExcModule);
            break;
        case CallbackQueueCall::CreateProcess:
            m_stopEventInProcess = CallbacksWorkerCreateProcess();
            break;
#ifdef INTEROP_DEBUGGING
        case CallbackQueueCall::InteropBreakpoint:
            m_stopEventInProcess = CallbacksWorkerInteropBreakpoint(c.pid, c.addr);
            break;
#endif // INTEROP_DEBUGGING
        default:
            // finish loop
            // called from destructor only, don't need call pop()
            return;
        }

        ToRelease<ICorDebugAppDomain> iCorAppDomain(c.iCorAppDomain.Detach());
        m_callbacksQueue.pop_front();

        // Continue process execution only in case we don't have stop event emitted and queue is empty.
        // We safe here against fast Continue()/AddCallbackToQueue() call from new callback call, since we don't unlock m_callbacksMutex.
        // m_callbacksMutex will be unlocked only in m_callbacksCV.wait(), when CallbacksWorker will be ready for notify_one.
        if (m_callbacksQueue.empty() && !m_stopEventInProcess)
        {
#ifdef INTEROP_DEBUGGING
            if (m_debugger.m_interopDebugging)
                m_debugger.m_uniqueInteropDebugger->ContinueAllThreadsWithEvents();

            if (iCorAppDomain) // last stop event was managed
            {
                iCorAppDomain->Continue(0);
            }
            else // last stop event was native
            {
                m_debugger.m_debugProcessRWLock.reader.lock();
                if (m_debugger.m_iCorProcess)
                {
                    m_debugger.m_iCorProcess->Continue(0);
                }
                m_debugger.m_debugProcessRWLock.reader.unlock();
            }
#else
            iCorAppDomain->Continue(0);
#endif // INTEROP_DEBUGGING
        }
    }
}

bool CallbacksQueue::HasQueuedCallbacks(ICorDebugProcess *pProcess)
{
    BOOL bQueued = FALSE;
    pProcess->HasQueuedCallbacks(NULL, &bQueued);
    return bQueued == TRUE;
}

HRESULT CallbacksQueue::AddCallbackToQueue(ICorDebugAppDomain *pAppDomain, std::function<void()> callback)
{
    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    callback();
    assert(!m_callbacksQueue.empty());

    // Note, we don't check m_callbacksQueue.empty() here, since callback() must add entry to queue.
    ToRelease<ICorDebugProcess> iCorProcess;
    if (SUCCEEDED(pAppDomain->GetProcess(&iCorProcess)) && HasQueuedCallbacks(iCorProcess))
        pAppDomain->Continue(0);
    else
        m_callbacksCV.notify_one(); // notify_one with lock

    return S_OK;
}

HRESULT CallbacksQueue::ContinueAppDomain(ICorDebugAppDomain *pAppDomain)
{
    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        if (!pAppDomain)
            return E_NOTIMPL;

        pAppDomain->Continue(0);
        return S_OK;
    }

    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    ToRelease<ICorDebugProcess> iCorProcess;
    if (m_callbacksQueue.empty() || (pAppDomain && SUCCEEDED(pAppDomain->GetProcess(&iCorProcess)) && HasQueuedCallbacks(iCorProcess)))
    {
        if (!pAppDomain)
            return E_NOTIMPL;

        pAppDomain->Continue(0);
    }
    else
        m_callbacksCV.notify_one(); // notify_one with lock

    return S_OK;
}

HRESULT CallbacksQueue::ContinueProcess(ICorDebugProcess *pProcess)
{
    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        if (!pProcess)
            return E_NOTIMPL;

        pProcess->Continue(0);
        return S_OK;
    }

    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    if (m_callbacksQueue.empty() || (pProcess && HasQueuedCallbacks(pProcess)))
    {
        if (!pProcess)
            return E_NOTIMPL;

        pProcess->Continue(0);
    }
    else
        m_callbacksCV.notify_one(); // notify_one with lock

    return S_OK;
}

bool CallbacksQueue::IsRunning()
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);
    return !m_stopEventInProcess;
}

HRESULT CallbacksQueue::Continue(ICorDebugProcess *pProcess)
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    assert(m_stopEventInProcess);
    m_stopEventInProcess = false;

    if (m_callbacksQueue.empty())
    {
#ifdef INTEROP_DEBUGGING
        if (m_debugger.m_interopDebugging)
            m_debugger.m_uniqueInteropDebugger->ContinueAllThreadsWithEvents();
#endif // INTEROP_DEBUGGING

        return pProcess->Continue(0);
    }

    m_callbacksCV.notify_one(); // notify_one with lock
    return S_OK;
}

// Caller should care about m_callbacksMutex.
// Check stop status and stop, if need.
// Return S_FALSE in case already was stopped, S_OK in case stopped by this call.
static HRESULT InternalStop(ICorDebugProcess *pProcess, bool &stopEventInProcess)
{
    if (stopEventInProcess)
        return S_FALSE; // Already stopped.

    HRESULT Status;
    IfFailRet(pProcess->Stop(0));
    stopEventInProcess = true;
    return S_OK;
}

// Analog of "pProcess->Stop(0)" call that also care about callbacks.
HRESULT CallbacksQueue::Stop(ICorDebugProcess *pProcess)
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);
    // DO NOT reset steppers here, this is "pProcess->Stop(0)" like call, that care about callbacks.
    return InternalStop(pProcess, m_stopEventInProcess);
}

// Stop process and set last stopped thread. If `lastStoppedThread` not passed value from protocol, find best thread.
HRESULT CallbacksQueue::Pause(ICorDebugProcess *pProcess, ThreadId lastStoppedThread)
{
    // Must be real thread ID or ThreadId::AllThreads.
    if (!lastStoppedThread)
        return E_INVALIDARG;

    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    // Note, in case Stop() failed, no stop event will be emitted, don't set m_stopEventInProcess to "true" in this case.
    HRESULT Status;
    IfFailRet(InternalStop(pProcess, m_stopEventInProcess));
    if (Status == S_FALSE) // Already stopped.
        return S_OK;

    // Same logic as provide vsdbg in case of pause during stepping.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(pProcess);

    // For Visual Studio, we have to report a thread ID in async stop event.
    // We have to find a thread which has a stack frame with valid location in its stack trace.
    std::vector<Thread> threads;
    m_debugger.GetThreads(threads);

    // In case `lastStoppedThread` provided, just check that we really have it.
    if (lastStoppedThread != ThreadId::AllThreads)
    {
        // In case VSCode protocol, user provide "pause" thread id.
        if (std::find_if(threads.begin(), threads.end(), [&](Thread t){ return t.id == lastStoppedThread; }) != threads.end())
        {
            // VSCode protocol event must provide thread only (VSCode count on this), even if this thread don't have user code.
            m_debugger.SetLastStoppedThreadId(lastStoppedThread);
            m_debugger.m_sharedProtocol->EmitStoppedEvent(StoppedEvent(StopPause, lastStoppedThread));
            m_debugger.m_ioredirect.async_cancel();
            return S_OK;
        }
    }
    else
    {
        // MI and CLI protocols provide ThreadId::AllThreads as lastStoppedThread, this protocols require thread and frame with user code.
        // Note, MIEngine (MI/GDB) require frame connected to user source or it will crash Visual Studio.

        ThreadId lastStoppedId = m_debugger.GetLastStoppedThreadId();

        // Reorder threads so that last stopped thread is checked first
        for (size_t i = 0; i < threads.size(); ++i)
        {
            if (threads[i].id == lastStoppedId)
            {
                std::swap(threads[0], threads[i]);
                break;
            }
        }

        // Now get stack trace for each thread and find a frame with valid source location.
        for (const Thread& thread : threads)
        {
            int totalFrames = 0;
            std::vector<StackFrame> stackFrames;

            if (FAILED(m_debugger.GetStackTrace(thread.id, FrameLevel(0), 0, stackFrames, totalFrames)))
                continue;

            for (const StackFrame& stackFrame : stackFrames)
            {
                if (stackFrame.source.IsNull())
                    continue;

                StoppedEvent event(StopPause, thread.id);
                event.frame = stackFrame;
                m_debugger.SetLastStoppedThreadId(thread.id);
                m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
                m_debugger.m_ioredirect.async_cancel();
                return S_OK;
            }
        }
    }

    // Fatal error during stop, just fail Pause request and don't stop process.
    m_stopEventInProcess = false;
    IfFailRet(pProcess->Continue(0));
    return E_FAIL;
}

CallbacksQueue::~CallbacksQueue()
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    // Clear queue and do notify_one call with FinishWorker request.
    m_callbacksQueue.clear();
    m_callbacksQueue.emplace_front(CallbackQueueCall::FinishWorker, nullptr, nullptr, nullptr, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
    m_stopEventInProcess = false; // forced to proceed during brake too
    m_callbacksCV.notify_one(); // notify_one with lock
    lock.unlock();
    m_callbacksWorker.join();
}

// NOTE caller must care about m_callbacksMutex.
void CallbacksQueue::EmplaceBack(CallbackQueueCall Call, ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint,
                                 CorDebugStepReason Reason, ExceptionCallbackType EventType, const std::string &ExcModule)
{
    m_callbacksQueue.emplace_back(Call, pAppDomain, pThread, pBreakpoint, Reason, EventType, ExcModule);
}

#ifdef INTEROP_DEBUGGING
bool CallbacksQueue::CallbacksWorkerInteropBreakpoint(pid_t pid, std::uintptr_t brkAddr)
{
    ThreadId threadId(pid);
    StoppedEvent event(StopBreakpoint, threadId);
    if (!m_debugger.m_sharedBreakpoints->IsInteropLineBreakpoint(brkAddr, event.breakpoint))
        return false;

    // Disable all steppers if we stop at breakpoint during step.
    m_debugger.m_debugProcessRWLock.reader.lock();
    if (m_debugger.m_iCorProcess)
    {
        m_debugger.m_uniqueSteppers->DisableAllSteppers(m_debugger.m_iCorProcess);
    }
    m_debugger.m_debugProcessRWLock.reader.unlock();

    m_debugger.SetLastStoppedThreadId(ThreadId(pid));

    if (FAILED(m_debugger.m_uniqueInteropDebugger->GetFrameForAddr(brkAddr, event.frame)))
    {
        event.frame.source = event.breakpoint.source;
        event.frame.line = event.breakpoint.line;
    }

    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return true;
}

HRESULT CallbacksQueue::AddInteropCallbackToQueue(std::function<void()> callback)
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    callback(); // Caller could add entries into m_callbacksQueue (this is why m_callbacksMutex cover this call).

    if (!m_callbacksQueue.empty())
    {
        // NOTE
        // In case `m_stopEventInProcess` is `true`, process have "stopped" status for sure, but could already execute some eval (this is OK, do not stop managed part!).
        // No need to check `IsEvalRunning()` here, since this code covered by `m_callbacksMutex` (that mean, breakpoint condition check with eval not running now for sure).
        if (!m_stopEventInProcess)
        {
            BOOL procRunning = FALSE;
            m_debugger.m_debugProcessRWLock.reader.lock();
            if (m_debugger.m_iCorProcess && SUCCEEDED(m_debugger.m_iCorProcess->IsRunning(&procRunning)) && procRunning == TRUE)
            {
                m_debugger.m_iCorProcess->Stop(0);
            }
            m_debugger.m_debugProcessRWLock.reader.unlock();
        }

        m_callbacksCV.notify_one(); // notify_one with lock
    }

    return S_OK;
}

// NOTE caller must care about m_callbacksMutex.
void CallbacksQueue::EmplaceBackInterop(CallbackQueueCall Call, pid_t pid, std::uintptr_t addr)
{
    m_callbacksQueue.emplace_back(Call, pid, addr);
}
#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
