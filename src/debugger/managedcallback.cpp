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
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "interfaces/iprotocol.h"
#include "utils/utf.h"

namespace netcoredbg
{

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

HRESULT STDMETHODCALLTYPE ManagedCallback::Breakpoint(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugBreakpoint *pBreakpoint)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    HRESULT Status;
    if (SUCCEEDED(Status = m_debugger.m_uniqueSteppers->ManagedCallbackBreakpoint(pAppDomain, pThread)) &&
        Status == S_OK) // S_FALSE - no error, but steppers not affect on callback
        return S_OK;

    ToRelease<ICorDebugAppDomain> callbackAppDomain(pAppDomain);
    pAppDomain->AddRef();
    ToRelease<ICorDebugThread> callbackThread(pThread);
    pThread->AddRef();
    ToRelease<ICorDebugBreakpoint> callbackBreakpoint(pBreakpoint);
    pBreakpoint->AddRef();

    std::thread([this](
        ICorDebugAppDomain *pAppDomain,
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint)
    {
        ThreadId threadId(getThreadId(pThread));
        bool atEntry = false;
        StoppedEvent event(StopBreakpoint, threadId);
        HRESULT Status;
        if (FAILED(Status = m_debugger.m_uniqueBreakpoints->ManagedCallbackBreakpoint(&m_debugger, pThread, pBreakpoint, event.breakpoint, atEntry)) ||
            Status == S_FALSE)
        {
            pAppDomain->Continue(0);
            return;
        }

        // Disable all steppers if we stop at breakpoint during step.
        m_debugger.m_uniqueSteppers->DisableAllSteppers(m_debugger.m_iCorProcess);

        if (atEntry)
            event.reason = StopEntry;

        ToRelease<ICorDebugFrame> pFrame;
        if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
            m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), event.frame);

        m_debugger.SetLastStoppedThread(pThread);
        m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
        m_debugger.m_ioredirect.async_cancel();
    },
        std::move(callbackAppDomain),
        std::move(callbackThread),
        std::move(callbackBreakpoint)
    ).detach();

    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::StepComplete(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugStepper *pStepper,
    /* [in] */ CorDebugStepReason reason)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));

    // Don't call DisableAllSteppers() or DisableAllSimpleSteppers() here!
    HRESULT Status;
    if (SUCCEEDED(Status = m_debugger.m_uniqueSteppers->ManagedCallbackStepComplete(pAppDomain, pThread, reason)) &&
        Status == S_OK) // S_FALSE - no error, but steppers not affect on callback
        return S_OK;

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> iCorFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&iCorFrame)) && iCorFrame != nullptr)
        m_debugger.GetFrameLocation(iCorFrame, threadId, FrameLevel(0), stackFrame);

    StoppedEvent event(StopStep, threadId);
    event.frame = stackFrame;

    m_debugger.SetLastStoppedThread(pThread);
    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();

    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Break(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    HRESULT Status;
    if (SUCCEEDED(Status = m_debugger.m_uniqueBreakpoints->ManagedCallbackBreak(pAppDomain, pThread, m_debugger.GetLastStoppedThreadId())) &&
        Status == S_OK) // S_FALSE - no error, but not affect on callback
        return S_OK;

    // Disable all steppers if we stop at break during step.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(m_debugger.m_iCorProcess);

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
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Exception(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ BOOL unhandled)
{
    // Obsolete callback
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EvalComplete(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugEval *pEval)
{
    LogFuncEntry();

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, pEval);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EvalException(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugEval *pEval)
{
    LogFuncEntry();

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, pEval);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateProcess(
    /* [in] */ ICorDebugProcess *pProcess)
{
    LogFuncEntry();
    m_debugger.NotifyProcessCreated();
    pProcess->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitProcess(
    /* [in] */ ICorDebugProcess *pProcess)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        LOGW("The target process exited while evaluating the function.");
    }

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

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateThread(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
        LOGW("Thread was created by user code during evaluation with implicit user code execution.");

    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Add(threadId);

    m_debugger.m_sharedProtocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitThread(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Remove(threadId);

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, nullptr);
    if (m_debugger.GetLastStoppedThreadId() == threadId)
    {
        m_debugger.InvalidateLastStoppedThreadId();
    }
    m_debugger.m_sharedProtocol->EmitThreadEvent(ThreadEvent(ThreadExited, threadId));
    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadModule(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugModule *pModule)
{
    LogFuncEntry();

    Module module;

    m_debugger.m_sharedModules->TryLoadModuleSymbols(pModule, module, m_debugger.IsJustMyCode());
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
        m_debugger.SetEnableCustomNotification(TRUE);

    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadModule(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugModule *pModule)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadClass(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugClass *c)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadClass(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugClass *c)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DebuggerError(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ HRESULT errorHR,
    /* [in] */ DWORD errorCode)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LogMessage(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ LONG lLevel,
    /* [in] */ WCHAR *pLogSwitchName,
    /* [in] */ WCHAR *pMessage)
{
    LogFuncEntry();
    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0);
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
    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LogSwitch(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ LONG lLevel,
    /* [in] */ ULONG ulReason,
    /* [in] */ WCHAR *pLogSwitchName,
    /* [in] */ WCHAR *pParentName)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateAppDomain(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitAppDomain(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadAssembly(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadAssembly(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ControlCTrap(
    /* [in] */ ICorDebugProcess *pProcess)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::NameChange(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UpdateModuleSymbols(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugModule *pModule,
    /* [in] */ IStream *pSymbolStream)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EditAndContinueRemap(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugFunction *pFunction,
    /* [in] */ BOOL fAccurate)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::BreakpointSetError(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugBreakpoint *pBreakpoint,
    /* [in] */ DWORD dwError)
{
    LogFuncEntry();
    return E_NOTIMPL;
}


// ICorDebugManagedCallback2

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapOpportunity(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugFunction *pOldFunction,
    /* [in] */ ICorDebugFunction *pNewFunction,
    /* [in] */ ULONG32 oldILOffset)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateConnection(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ CONNID dwConnectionId,
    /* [in] */ WCHAR *pConnName)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ChangeConnection(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ CONNID dwConnectionId)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DestroyConnection(
    /* [in] */ ICorDebugProcess *pProcess,
    /* [in] */ CONNID dwConnectionId)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Exception(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugFrame *pFrame,
    /* [in] */ ULONG32 nOffset,
    /* [in] */ CorDebugExceptionCallbackType dwEventType,
    /* [in] */ DWORD dwFlags)
{
    LogFuncEntry();

    // In case we inside evaluation (exception during implicit function execution), make sure we continue process execution.
    // This is internal CoreCLR routine, should not be interrupted by debugger. CoreCLR will care about exception in this case
    // and provide exception data as evaluation result.
    // Note, we may have issue here, if during eval new thread was created within unhandled exception.
    // In this case we have same behaviour as MS vsdbg and MSVS C# debugger - debuggee process terminated by runtime, debug session ends.
    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopException, threadId);

    HRESULT Status;
    std::string textOutput;
    if (FAILED(Status = m_debugger.m_uniqueBreakpoints->ManagedCallbackException(pThread, dwEventType, event, textOutput)) ||
        Status == S_FALSE)
    {
        // TODO why we mixing debugger's output with user application output???
        m_debugger.m_sharedProtocol->EmitOutputEvent(OutputConsole, textOutput, "target-exception");
        pAppDomain->Continue(0);
        return S_OK;
    }

    ToRelease<ICorDebugFrame> pActiveFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pActiveFrame)) && pActiveFrame != nullptr)
        m_debugger.GetFrameLocation(pActiveFrame, threadId, FrameLevel(0), event.frame);

    m_debugger.SetLastStoppedThread(pThread);

    // TODO this looks like wrong logic with m_stopCounter, we must not have situation when process stop logic emit continued event.
    //      Exception breakpoint related code must be refactored.
    m_debugger.m_stopCounterMutex.lock();
    while (m_debugger.m_stopCounter > 0) {
        m_debugger.m_sharedProtocol->EmitContinuedEvent(m_debugger.GetLastStoppedThreadId());
        --m_debugger.m_stopCounter;
    }
    // INFO: Double EmitStopEvent() produce blocked coreclr command reader
    m_debugger.m_stopCounter = 1;
    m_debugger.m_stopCounterMutex.unlock();

    m_debugger.m_sharedProtocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExceptionUnwind(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
    /* [in] */ DWORD dwFlags)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));
    // We produce DEBUG_EXCEPTION_INTERCEPTED from Exception() callback.
    // TODO: we should waiting this unwinding on exit().
    LOGI("ExceptionUnwind:threadId:%d,dwEventType:%d,dwFlags:%d", int(threadId), dwEventType, dwFlags);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapComplete(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugFunction *pFunction)
{
    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::MDANotification(
    /* [in] */ ICorDebugController *pController,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugMDA *pMDA)
{
    // TODO: MDA notification should be supported with exception breakpoint feature (MDA enabled only under Microsoft Windows OS)
    // https://docs.microsoft.com/ru-ru/dotnet/framework/unmanaged-api/debugging/icordebugmanagedcallback2-mdanotification-method
    // https://docs.microsoft.com/ru-ru/dotnet/framework/debug-trace-profile/diagnosing-errors-with-managed-debugging-assistants#enable-and-disable-mdas
    //

    LogFuncEntry();
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CustomNotification(
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();

    m_debugger.m_sharedEvalWaiter->ManagedCallbackCustomNotification(pThread);

    pAppDomain->Continue(0);
    return S_OK;
}

} // namespace netcoredbg
