// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/managedcallback.h"

#include "debugger/threads.h"
#include "debugger/evalwaiter.h"
#include "debugger/breakpoints.h"
#include "debugger/valueprint.h"
#include "debugger/waitpid.h"
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "interfaces/iprotocol.h"
#include "protocols/protocol.h"
#include "utils/utf.h"

namespace netcoredbg
{

static HRESULT GetExceptionInfo(ICorDebugThread *pThread,
                                std::string &excType,
                                std::string &excModule)
{
    HRESULT Status;

    ToRelease<ICorDebugValue> pExceptionValue;
    IfFailRet(pThread->GetCurrentException(&pExceptionValue));

    TypePrinter::GetTypeOfValue(pExceptionValue, excType);

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;
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
        Status == S_OK) // S_FAIL - no error, but steppers not affect on callback
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
        if (FAILED(m_debugger.m_uniqueBreakpoints->HitBreakpoint(&m_debugger, pThread, pBreakpoint, event.breakpoint, atEntry)))
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

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> pFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
        m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), stackFrame);

    // Don't call DisableAllSteppers() or DisableAllSimpleSteppers() here!
    HRESULT Status;
    if (SUCCEEDED(Status = m_debugger.m_uniqueSteppers->ManagedCallbackStepComplete(pAppDomain, pThread, reason, stackFrame)) &&
        Status == S_OK) // S_FAIL - no error, but steppers not affect on callback
        return S_OK;

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

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> pFrame;
    ThreadId threadId(getThreadId(pThread));
    if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
        m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), stackFrame);

    // No reason check GetFrameLocation() return code, since it could be failed by some API call after source detection.
    const bool no_source = stackFrame.source.IsNull();

    if (m_debugger.IsJustMyCode() && no_source)
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    // Prevent stop event duplicate, if previous stop event was for same thread and same code point.
    // The idea is - store "fully qualified IL offset" (data for module + method + IL) on any stop event
    // and only at Break() callback check the real sequence point (required delegate call).
    // Note, that step/breakpoint/etc stop event at "Debugger.Break()" source line and stop event
    // generated by CoreCLR during Debugger.Break() execution in managed code have different IL offsets,
    // but same sequence point.
    do 
    {
        std::lock_guard<std::mutex> lock(m_debugger.m_lastStoppedMutex);

        if (threadId != m_debugger.m_lastStoppedThreadId ||
            m_debugger.m_lastStoppedIlOffset.modAddress == 0 ||
            m_debugger.m_lastStoppedIlOffset.methodToken == 0)
            break;

        ManagedDebugger::FullyQualifiedIlOffset_t fullyQualifiedIlOffset;
        if (FAILED(m_debugger.GetFullyQualifiedIlOffset(threadId, fullyQualifiedIlOffset)))
            break;

        if (fullyQualifiedIlOffset.modAddress == 0 ||
            fullyQualifiedIlOffset.methodToken == 0 ||
            fullyQualifiedIlOffset.modAddress != m_debugger.m_lastStoppedIlOffset.modAddress ||
            fullyQualifiedIlOffset.methodToken != m_debugger.m_lastStoppedIlOffset.methodToken)
            break;

        Modules::SequencePoint lastSP;
        if (FAILED(m_debugger.m_sharedModules->GetSequencePointByILOffset(
                       m_debugger.m_lastStoppedIlOffset.modAddress,
                       m_debugger.m_lastStoppedIlOffset.methodToken,
                       m_debugger.m_lastStoppedIlOffset.ilOffset,
                       lastSP)))
        {
            LOGE("Can't get sequence point for m_lastStoppedIlOffset");
            break;
        }

        Modules::SequencePoint curSP;
        if (FAILED(m_debugger.m_sharedModules->GetSequencePointByILOffset(
                       fullyQualifiedIlOffset.modAddress,
                       fullyQualifiedIlOffset.methodToken,
                       fullyQualifiedIlOffset.ilOffset,
                       curSP)))
        {
            LOGE("Can't get sequence point for IL offset");
            break;
        }

        if (lastSP.startLine != curSP.startLine ||
            lastSP.startColumn != curSP.startColumn ||
            lastSP.endLine != curSP.endLine ||
            lastSP.endColumn != curSP.endColumn ||
            lastSP.offset != curSP.offset ||
            lastSP.document != curSP.document)
            break;

        pAppDomain->Continue(0);
        return S_OK;
    }
    while (0);

    // Disable all steppers if we stop at break during step.
    m_debugger.m_uniqueSteppers->DisableAllSteppers(m_debugger.m_iCorProcess);

    m_debugger.SetLastStoppedThread(pThread);

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
        m_debugger.m_uniqueBreakpoints->TryResolveBreakpointsForModule(pModule, events);
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

    // INFO: Exception event callbacks produce Stop process and managed threads in coreCLR
    // After emit Stop event from debugger coreclr by command handler send a ExceptionInfo request.
    // For answer on ExceptionInfo are needed long FuncEval() with asynchronous EvalComplete event.
    // Of course evaluations is not atomic for coreCLR. Before EvalComplete we can get a new
    // ExceptionEvent if we allow to running of current thread.
    //
    // Current implementation stops all threads while EvalComplete waiting. But, unfortunately,
    // it's not helps in any cases. Exceptions can be throws in the same time from some threads.
    // And in this case threads thread suspend is not guaranteed, becase thread can stay in
    // "GC unsafe mode" or "Optimized code". And also, same time exceptions puts in priority queue event,
    // and all next events one by one will transport each exception.
    // For "GC unsafe mode" or "Optimized code" we cannot invoke CreateEval() function.

    HRESULT Status;
    std::string excType, excModule;
    if (FAILED(Status = GetExceptionInfo(pThread, excType, excModule)))
    {
        excType = "unknown exception";
        excModule = "unknown module";
        LOGI("Can't get exception info: %s", errormessage(Status));
    }

    ExceptionBreakMode mode;
    m_debugger.m_uniqueBreakpoints->GetExceptionBreakMode(mode, "*");
    bool unhandled = (dwEventType == DEBUG_EXCEPTION_UNHANDLED && mode.Unhandled());
    bool not_matched = !(unhandled || m_debugger.MatchExceptionBreakpoint(dwEventType, excType, ExceptionBreakCategory::CLR));

    if (not_matched)
    {
        // TODO why we mixing debugger's output with user application output???
        std::string text = "Exception thrown: '" + excType + "' in " + excModule + "\n";
        m_debugger.m_sharedProtocol->EmitOutputEvent(OutputConsole, {&text[0], text.size()}, "target-exception");
        pAppDomain->Continue(0);
        return S_OK;
    }

    ThreadId threadId(getThreadId(pThread));
    StoppedEvent event(StopException, threadId);

    std::string details;
    if (unhandled)
    {
        details = "An unhandled exception of type '" + excType + "' occurred in " + excModule;
        std::lock_guard<std::mutex> lock(m_debugger.m_lastUnhandledExceptionThreadIdsMutex);
        m_debugger.m_lastUnhandledExceptionThreadIds.insert(threadId);
    }
    else
    {
        details = "Exception thrown: '" + excType + "' in " + excModule;
    }

    std::string message;
    WCHAR fieldName[] = W("_message\0");
    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)))
        PrintStringField(pExceptionValue, fieldName, message);

    ToRelease<ICorDebugFrame> pActiveFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pActiveFrame)) && pActiveFrame != nullptr)
        m_debugger.GetFrameLocation(pActiveFrame, threadId, FrameLevel(0), event.frame);

    m_debugger.SetLastStoppedThread(pThread);

    event.text = excType;
    event.description = message.empty() ? details : message;

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
