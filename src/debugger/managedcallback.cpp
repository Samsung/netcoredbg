// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifdef _MSC_VER
#include <wtypes.h>
#endif

#include "debugger/managedcallback.h"
#include "debugger/threads.h"
#include "debugger/evalwaiter.h"
#include "debugger/breakpoint_break.h"
#include "debugger/breakpoint_entry.h"
#include "debugger/breakpoints_exception.h"
#include "debugger/breakpoints_func.h"
#include "debugger/breakpoints_line.h"
#include "debugger/breakpoints.h"
#include "debugger/valueprint.h"
#include "debugger/waitpid.h"
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "debugger/evalstackmachine.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "interfaces/iprotocol.h"
#include "utils/utf.h"

namespace netcoredbg
{

bool ManagedCallback::CallbacksWorkerBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    // S_FALSE - not error and steppers not affect on callback
    if (S_FALSE != m_debugger.m_uniqueSteppers->ManagedCallbackBreakpoint(pAppDomain, pThread))
        return false;

    bool atEntry = false;
    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopBreakpoint, threadId);
    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_uniqueBreakpoints->ManagedCallbackBreakpoint(pThread, pBreakpoint, event.breakpoint, atEntry))
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

bool ManagedCallback::CallbacksWorkerStepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, CorDebugStepReason reason)
{
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

bool ManagedCallback::CallbacksWorkerBreak(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_uniqueBreakpoints->ManagedCallbackBreak(pThread, m_debugger.GetLastStoppedThreadId()))
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

bool ManagedCallback::CallbacksWorkerException(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ExceptionCallbackType eventType, const std::string &excModule)
{
    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopException, threadId);

    // S_FALSE - not error and not affect on callback (callback will emit stop event)
    if (S_FALSE != m_debugger.m_uniqueBreakpoints->ManagedCallbackException(pThread, eventType, excModule, event))
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

void ManagedCallback::CallbacksWorker()
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
            iCorAppDomain->Continue(0);
    }
}

bool ManagedCallback::HasQueuedCallbacks(ICorDebugProcess *pProcess)
{
    BOOL bQueued = FALSE;
    pProcess->HasQueuedCallbacks(NULL, &bQueued);
    return bQueued == TRUE;
}

HRESULT ManagedCallback::AddCallbackToQueue(ICorDebugAppDomain *pAppDomain, std::function<void()> callback)
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

HRESULT ManagedCallback::ContinueAppDomainWithCallbacksQueue(ICorDebugAppDomain *pAppDomain)
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

HRESULT ManagedCallback::ContinueProcessWithCallbacksQueue(ICorDebugProcess *pProcess)
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

bool ManagedCallback::IsRunning()
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);
    return !m_stopEventInProcess;
}

HRESULT ManagedCallback::Continue(ICorDebugProcess *pProcess)
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    assert(m_stopEventInProcess);
    m_stopEventInProcess = false;

    if (m_callbacksQueue.empty())
        return pProcess->Continue(0);

    m_callbacksCV.notify_one(); // notify_one with lock
    return S_OK;
}

HRESULT ManagedCallback::Pause(ICorDebugProcess *pProcess)
{
    std::unique_lock<std::mutex> lock(m_callbacksMutex);

    if (m_stopEventInProcess)
        return S_OK;

    // Note, in case Stop() failed, no stop event will be emitted, don't set m_stopEventInProcess to "true" in this case.
    HRESULT Status;
    if (FAILED(Status = pProcess->Stop(0)))
        return Status;

    m_stopEventInProcess = true;

    // Same logic as provide vsdbg in case of pause during stepping.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(pProcess);

    // For Visual Studio, we have to report a thread ID in async stop event.
    // We have to find a thread which has a stack frame with valid location in its stack trace.
    std::vector<Thread> threads;
    m_debugger.GetThreads(threads);

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

    assert(threads.size() > 0);
    // Event must provide thread (VSCode and MI/GDB protocols count on this), even if this thread don't have user code.
    // Note, provide thread without user code also legit for this event.
    m_debugger.m_sharedProtocol->EmitStoppedEvent(StoppedEvent(StopPause, threads[0].id));
    m_debugger.m_ioredirect.async_cancel();
    return S_OK;
}

ManagedCallback::~ManagedCallback()
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

ULONG ManagedCallback::GetRefCount()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_refCountMutex);
    return m_refCount;
}


// IUnknown

HRESULT STDMETHODCALLTYPE ManagedCallback::QueryInterface(REFIID riid, VOID** ppInterface)
{
    LogFuncEntry();

    if (riid == IID_ICorDebugManagedCallback)
    {
        *ppInterface = static_cast<ICorDebugManagedCallback*>(this);
    }
    else if (riid == IID_ICorDebugManagedCallback2)
    {
        *ppInterface = static_cast<ICorDebugManagedCallback2*>(this);
    }
    else if (riid == IID_ICorDebugManagedCallback3)
    {
        *ppInterface = static_cast<ICorDebugManagedCallback3*>(this);
    }
    else if (riid == IID_IUnknown)
    {
        *ppInterface = static_cast<IUnknown*>(static_cast<ICorDebugManagedCallback*>(this));
    }
    else
    {
        *ppInterface = NULL;
        return E_NOINTERFACE;
    }

    this->AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE ManagedCallback::AddRef()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_refCountMutex);
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE ManagedCallback::Release()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_refCountMutex);

    assert(m_refCount > 0);

    // Note, we don't provide "delete" call for object itself for our fake "COM".
    // External holder will care about this object during debugger lifetime.

    return --m_refCount;
}


// ICorDebugManagedCallback

HRESULT STDMETHODCALLTYPE ManagedCallback::Breakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    LogFuncEntry();
    return AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        pBreakpoint->AddRef();
        m_callbacksQueue.emplace_back(CallbackQueueCall::Breakpoint, pAppDomain, pThread, pBreakpoint, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::StepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                        ICorDebugStepper *pStepper, CorDebugStepReason reason)
{
    LogFuncEntry();
    return AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        m_callbacksQueue.emplace_back(CallbackQueueCall::StepComplete, pAppDomain, pThread, nullptr, reason, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Break(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();
    return AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        m_callbacksQueue.emplace_back(CallbackQueueCall::Break, pAppDomain, pThread, nullptr, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Exception(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, BOOL unhandled)
{
    // Obsolete callback
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EvalComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugEval *pEval)
{
    LogFuncEntry();
    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, pEval);
    return S_OK; // Eval-related routine - no callbacks queue related code here.
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EvalException(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugEval *pEval)
{
    LogFuncEntry();
    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, pEval);
    return S_OK; // Eval-related routine - no callbacks queue related code here.
}

// https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/icordebugmanagedcallback-createprocess-method
// Notifies the debugger when a process has been attached or started for the first time.
HRESULT STDMETHODCALLTYPE ManagedCallback::CreateProcess(ICorDebugProcess *pProcess)
{
    LogFuncEntry();
    m_debugger.NotifyProcessCreated();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitProcess(ICorDebugProcess *pProcess)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
        LOGW("The target process exited while evaluating the function.");

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(nullptr, nullptr);

    // Linux: exit() and _exit() argument is int (signed int)
    // Windows: ExitProcess() and TerminateProcess() argument is UINT (unsigned int)
    // Windows: GetExitCodeProcess() argument is DWORD (unsigned long)
    // internal CoreCLR variable LatchedExitCode is INT32 (signed int)
    // C# Main() return values is int (signed int) or void (return 0)
    int exitCode = 0;
#ifdef FEATURE_PAL
    exitCode = GetWaitpid().GetExitCode();
#else
    HPROCESS hProcess;
    DWORD dwExitCode = 0;
    if (SUCCEEDED(pProcess->GetHandle(&hProcess)))
    {
        GetExitCodeProcess(hProcess, &dwExitCode);
        exitCode = static_cast<int>(dwExitCode);
    }
#endif // FEATURE_PAL

    m_debugger.m_sharedProtocol->EmitExitedEvent(ExitedEvent(exitCode));
    m_debugger.NotifyProcessExited();
    m_debugger.m_sharedProtocol->EmitTerminatedEvent();
    m_debugger.m_ioredirect.async_cancel();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
        LOGW("Thread was created by user code during evaluation with implicit user code execution.");

    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Add(threadId);

    m_debugger.m_sharedProtocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();

    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Remove(threadId);

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, nullptr);
    if (m_debugger.GetLastStoppedThreadId() == threadId)
        m_debugger.InvalidateLastStoppedThreadId();

    m_debugger.m_uniqueBreakpoints->ManagedCallbackExitThread(pThread);

    m_debugger.m_sharedProtocol->EmitThreadEvent(ThreadEvent(ThreadExited, threadId));
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule)
{
    LogFuncEntry();

    Module module;
    std::string outputText;
    m_debugger.m_sharedModules->TryLoadModuleSymbols(pModule, module, m_debugger.IsJustMyCode(), m_debugger.IsHotReload(), outputText);
    if (!outputText.empty())
        m_debugger.m_sharedProtocol->EmitOutputEvent(OutputStdErr, outputText);
    m_debugger.m_sharedProtocol->EmitModuleEvent(ModuleEvent(ModuleNew, module));

    if (module.symbolStatus == SymbolsLoaded)
    {
        std::vector<BreakpointEvent> events;
        m_debugger.m_uniqueBreakpoints->ManagedCallbackLoadModule(pModule, events);
        for (const BreakpointEvent &event : events)
            m_debugger.m_sharedProtocol->EmitBreakpointEvent(event);
    }

    // enable Debugger.NotifyOfCrossThreadDependency after System.Private.CoreLib.dll loaded (trigger for 1 time call only)
    if (module.name == "System.Private.CoreLib.dll")
    {
        m_debugger.SetEnableCustomNotification(TRUE);
        m_debugger.m_sharedEvalStackMachine->FindPredefinedTypes(pModule);
    }

    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DebuggerError(ICorDebugProcess *pProcess, HRESULT errorHR, DWORD errorCode)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LogMessage(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                      LONG lLevel, WCHAR *pLogSwitchName, WCHAR *pMessage)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0); // Eval-related routine - ignore callbacks queue, continue process execution.
        return S_OK;
    }

    string_view src;
    std::string src_holder;
    if (pLogSwitchName && *pLogSwitchName)
    {
        src_holder = to_utf8(pLogSwitchName);
        src = src_holder;
    }
    else
    {
        src = "Debugger.Log";
    }

    m_debugger.m_sharedProtocol->EmitOutputEvent(OutputConsole, to_utf8(pMessage), src);
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LogSwitch(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, LONG lLevel,
                                                     ULONG ulReason, WCHAR *pLogSwitchName, WCHAR *pParentName)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ControlCTrap(ICorDebugProcess *pProcess)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::NameChange(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UpdateModuleSymbols(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule, IStream *pSymbolStream)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EditAndContinueRemap(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                                ICorDebugFunction *pFunction, BOOL fAccurate)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::BreakpointSetError(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                              ICorDebugBreakpoint *pBreakpoint, DWORD dwError)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}


// ICorDebugManagedCallback2

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapOpportunity(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                                    ICorDebugFunction *pOldFunction, ICorDebugFunction *pNewFunction, ULONG32 oldILOffset)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId, WCHAR *pConnName)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ChangeConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DestroyConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId)
{
    LogFuncEntry();
    return ContinueProcessWithCallbacksQueue(pProcess);
}

static HRESULT GetExceptionModuleName(ICorDebugFrame *pFrame, std::string &excModule)
{
    // Exception was thrown outside of managed code (for example, by runtime).
    if (pFrame == nullptr)
    {
        excModule = "<unknown module>";
        return S_OK;
    }

    HRESULT Status;
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    WCHAR mdName[mdNameLen];
    ULONG nameLen;
    IfFailRet(pMDImport->GetScopeProps(mdName, _countof(mdName), &nameLen, nullptr));
    excModule = to_utf8(mdName);

    return S_OK;
}

static ExceptionCallbackType CorrectedByJMCCatchHandlerEventType(ICorDebugFrame *pFrame, bool justMyCode)
{
    if (!justMyCode)
        return ExceptionCallbackType::CATCH_HANDLER_FOUND;

    BOOL JMCStatus = FALSE;
    ToRelease<ICorDebugFunction> iCorFunction;
    ToRelease<ICorDebugFunction2> iCorFunction2;
    if (pFrame != nullptr &&
        SUCCEEDED(pFrame->GetFunction(&iCorFunction)) &&
        SUCCEEDED(iCorFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID*) &iCorFunction2)) &&
        SUCCEEDED(iCorFunction2->GetJMCStatus(&JMCStatus)) &&
        JMCStatus == TRUE)
    {
        return ExceptionCallbackType::USER_CATCH_HANDLER_FOUND;
    }

    return ExceptionCallbackType::CATCH_HANDLER_FOUND;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Exception(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugFrame *pFrame,
                                                     ULONG32 nOffset, CorDebugExceptionCallbackType dwEventType, DWORD dwFlags)
{
    LogFuncEntry();
    return AddCallbackToQueue(pAppDomain, [&]()
    {
        // pFrame could be neutered in case of evaluation during brake, do all stuff with pFrame in callback itself.
        ExceptionCallbackType eventType;
        std::string excModule;
        switch(dwEventType)
        {
        case DEBUG_EXCEPTION_FIRST_CHANCE:
            eventType = ExceptionCallbackType::FIRST_CHANCE;
            GetExceptionModuleName(pFrame, excModule);
            break;
        case DEBUG_EXCEPTION_USER_FIRST_CHANCE:
            eventType = ExceptionCallbackType::USER_FIRST_CHANCE;
            GetExceptionModuleName(pFrame, excModule);
            break;
        case DEBUG_EXCEPTION_CATCH_HANDLER_FOUND:
            eventType = CorrectedByJMCCatchHandlerEventType(pFrame, m_debugger.IsJustMyCode());
            break;
        default:
            eventType = ExceptionCallbackType::UNHANDLED;
            break;
        }

        pAppDomain->AddRef();
        pThread->AddRef();
        m_callbacksQueue.emplace_back(CallbackQueueCall::Exception, pAppDomain, pThread, nullptr, STEP_NORMAL, eventType, excModule);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExceptionUnwind(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                           CorDebugExceptionUnwindCallbackType dwEventType, DWORD dwFlags)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugFunction *pFunction)
{
    LogFuncEntry();
    return ContinueAppDomainWithCallbacksQueue(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::MDANotification(ICorDebugController *pController, ICorDebugThread *pThread, ICorDebugMDA *pMDA)
{
    LogFuncEntry();
    ToRelease<ICorDebugProcess> iCorProcess;
    pThread->GetProcess(&iCorProcess);
    return ContinueProcessWithCallbacksQueue(iCorProcess);
}


// ICorDebugManagedCallback3

HRESULT STDMETHODCALLTYPE ManagedCallback::CustomNotification(ICorDebugThread *pThread, ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    m_debugger.m_sharedEvalWaiter->ManagedCallbackCustomNotification(pThread);
    pAppDomain->Continue(0); // Eval-related routine - ignore callbacks queue, continue process execution.
    return S_OK;
}

} // namespace netcoredbg
