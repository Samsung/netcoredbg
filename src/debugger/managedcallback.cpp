// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/managedcallback.h"

#include "debugger/valueprint.h"
#include "debugger/waitpid.h"
#include "metadata/typeprinter.h"
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

void ManagedCallback::HandleEvent(ICorDebugController *controller, const std::string &eventName)
{
    LogFuncEntry();

    // TODO why we mixing debugger's output with user application output???
    std::string text = "Event received: '" + eventName + "'\n"; 
    m_debugger.m_protocol->EmitOutputEvent(OutputConsole, {&text[0], text.size()});
    controller->Continue(0);
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
    ThreadId threadId(getThreadId(pThread));

    if (m_debugger.m_evaluator.IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    // Check async stepping related breakpoints first, since user can't setup breakpoints to await block yield or resume offsets manually,
    // so, async stepping related breakpoints not a part of any user breakpoints related data (that will be checked in separate thread. see code below).
    if (m_debugger.HitAsyncStepBreakpoint(pAppDomain, pThread, pBreakpoint))
        return S_OK;

    auto stepForcedIgnoreBP = [&] () {
        {
            std::lock_guard<std::mutex> lock(m_debugger.m_stepMutex);
            if (m_debugger.m_enabledSimpleStepId != int(threadId))
            {
                return false;
            }
        }

        ToRelease<ICorDebugStepperEnum> steppers;
        if (FAILED(pAppDomain->EnumerateSteppers(&steppers)))
            return false;

        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            BOOL pbActive;
            ToRelease<ICorDebugStepper> pStepper(curStepper);
            if (SUCCEEDED(pStepper->IsActive(&pbActive)) && pbActive)
                return false;
        }

        return true;
    };

    if (stepForcedIgnoreBP())
    {
        pAppDomain->Continue(0);
        return S_OK;  
    }

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
        if (FAILED(m_debugger.m_breakpoints.HitBreakpoint(&m_debugger, pThread, pBreakpoint, event.breakpoint, atEntry)))
        {
            pAppDomain->Continue(0);
            return;
        }

        // Disable all steppers if we stop at breakpoint during step.
        m_debugger.DisableAllSteppers();

        if (atEntry)
            event.reason = StopEntry;

        ToRelease<ICorDebugFrame> pFrame;
        if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
            m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), event.frame);

        m_debugger.SetLastStoppedThread(pThread);
        m_debugger.m_protocol->EmitStoppedEvent(event);
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

    // Reset simple step without real stepper release.
    m_debugger.m_stepMutex.lock();
    m_debugger.m_enabledSimpleStepId = 0;
    m_debugger.m_stepMutex.unlock();
    // In case we have async method and first await breakpoint (yield_offset) was enabled, but not reached.
    m_debugger.m_asyncStepMutex.lock();
    if (m_debugger.m_asyncStep)
        m_debugger.m_asyncStep.reset(nullptr);
    m_debugger.m_asyncStepMutex.unlock();

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> pFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
        m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), stackFrame);

    // No reason check GetFrameLocation() return code, since it could be failed by some API call after source detection.
    const bool no_source = stackFrame.source.IsNull();

    if (m_debugger.IsJustMyCode() && no_source)
    {
        // FIXME will not work in proper way with async methods
        m_debugger.SetupSimpleStep(pThread, Debugger::STEP_OVER);
        pAppDomain->Continue(0);
        return S_OK;
    }

    // Note, step-in stop at the beginning of a newly called function at 0 IL offset.
    // But, for example, async function don't have user code at 0 IL offset, so,
    // we need additional step-over to real user code.
    if (reason == CorDebugStepReason::STEP_CALL)
    {
        HRESULT Status;
        ToRelease<ICorDebugFrame> pFrame;
        ToRelease<ICorDebugILFrame> pILFrame;
        ULONG32 ipOffset;
        CorDebugMappingResult mappingResult;
        Modules::SequencePoint sequencePoint;
        if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr &&
            SUCCEEDED(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame)) &&
            SUCCEEDED(pILFrame->GetIP(&ipOffset, &mappingResult)) &&
            SUCCEEDED(m_debugger.m_modules.GetFrameILAndSequencePoint(pFrame, ipOffset, sequencePoint)) &&
            // Current IL offset less than real offset of first user code line.
            ipOffset < (ULONG32)sequencePoint.offset)
        {
            IfFailRet(m_debugger.SetupSimpleStep(pThread, Debugger::STEP_OVER));
            pAppDomain->Continue(0);
            return S_OK;
        }
    }

    StoppedEvent event(StopStep, threadId);
    event.frame = stackFrame;

    m_debugger.SetLastStoppedThread(pThread);
    m_debugger.m_protocol->EmitStoppedEvent(event);
    m_debugger.m_ioredirect.async_cancel();

    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Break(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));

    if (m_debugger.m_evaluator.IsEvalRunning())
    {
        pAppDomain->Continue(0);
        return S_OK;
    }

    StackFrame stackFrame;
    ToRelease<ICorDebugFrame> pFrame;
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
        if (FAILED(m_debugger.m_modules.GetSequencePointByILOffset(
                       m_debugger.m_lastStoppedIlOffset.modAddress,
                       m_debugger.m_lastStoppedIlOffset.methodToken,
                       m_debugger.m_lastStoppedIlOffset.ilOffset,
                       lastSP)))
        {
            LOGE("Can't get sequence point for m_lastStoppedIlOffset");
            break;
        }

        Modules::SequencePoint curSP;
        if (FAILED(m_debugger.m_modules.GetSequencePointByILOffset(
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
    m_debugger.DisableAllSteppers();

    m_debugger.SetLastStoppedThread(pThread);

    StoppedEvent event(StopPause, threadId);
    event.frame = stackFrame;
    m_debugger.m_protocol->EmitStoppedEvent(event);
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
    ThreadId currentThreadId = getThreadId(pThread);

    HRESULT Status = S_OK;

    m_debugger.m_evaluator.NotifyEvalComplete(pThread, pEval);

    if (m_debugger.m_evaluator.is_empty_eval_queue())
    {
        pAppDomain->SetAllThreadsDebugState(THREAD_RUN, nullptr);
    }
    else
    {
        ThreadId evalThreadId = m_debugger.m_evaluator.front_eval_queue();
        if (evalThreadId == currentThreadId)
        {
            LOGI("Complete eval threadid = '%d'", int(currentThreadId));
            m_debugger.m_evaluator.pop_eval_queue();

            if (m_debugger.m_evaluator.is_empty_eval_queue())
            {
                pAppDomain->SetAllThreadsDebugState(THREAD_RUN, nullptr);
            }
            else
            {
                evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                ToRelease<ICorDebugThread> pThreadEval;
                IfFailRet(m_debugger.m_pProcess->GetThread(int(evalThreadId), &pThreadEval));
                IfFailRet(pAppDomain->SetAllThreadsDebugState(THREAD_SUSPEND, nullptr));
                IfFailRet(pThreadEval->SetDebugState(THREAD_RUN));
            }
        }
        else
        {
            LOGE("Logical error: eval queue '%d' != '%d'", int(currentThreadId), int(evalThreadId));
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EvalException(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread,
    /* [in] */ ICorDebugEval *pEval)
{
    LogFuncEntry();
    ThreadId currentThreadId = getThreadId(pThread);

    HRESULT Status = S_OK;

    // TODO: Need implementation
    //
    // This is callback EvalException invoked on evaluation interruption event.
    // And, evaluated results has inconsistent states. Notify is not enough for this point.

    m_debugger.m_evaluator.NotifyEvalComplete(pThread, pEval);

    // NOTE
    // In case of unhandled exception inside implicit function call (for example, getter),
    // ICorDebugManagedCallback::EvalException() is exit point for eval routine, make sure,
    // that proper threads states are setted up.
    if (m_debugger.m_evaluator.is_empty_eval_queue())
    {
        pAppDomain->SetAllThreadsDebugState(THREAD_RUN, nullptr);
    }
    else
    {
        ThreadId evalThreadId = m_debugger.m_evaluator.front_eval_queue();
        if (evalThreadId == currentThreadId)
        {
            m_debugger.m_evaluator.pop_eval_queue();
            LOGI("Eval exception, threadid = '%d'", int(currentThreadId));

            if (m_debugger.m_evaluator.is_empty_eval_queue())
            {
                pAppDomain->SetAllThreadsDebugState(THREAD_RUN, nullptr);
            }
            else
            {
                evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                ToRelease<ICorDebugThread> pThreadEval;
                IfFailRet(m_debugger.m_pProcess->GetThread(int(evalThreadId), &pThreadEval));
                IfFailRet(pAppDomain->SetAllThreadsDebugState(THREAD_SUSPEND, nullptr));
                IfFailRet(pThreadEval->SetDebugState(THREAD_RUN));
            }
        }
        else
        {
            LOGE("Logical error: eval queue '%d' != '%d'", int(currentThreadId), int(evalThreadId));
        }
    }

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

    if (m_debugger.m_evaluator.IsEvalRunning())
    {
        LOGW("The target process exited while evaluating the function.");
    }

    m_debugger.m_evaluator.NotifyEvalComplete(nullptr, nullptr);

    while (!m_debugger.m_evaluator.is_empty_eval_queue())
        m_debugger.m_evaluator.pop_eval_queue();

    // Linux: exit() and _exit() argument is int (signed int)
    // Windows: ExitProcess() and TerminateProcess() argument is UINT (unsigned int)
    // Windows: GetExitCodeProcess() argument is DWORD (unsigned long)
    // internal CoreCLR variable LatchedExitCode is INT32 (signed int)
    // C# Main() return values is int (signed int) or void (return 0)
    int exitCode = 0;
#ifdef FEATURE_PAL
    exitCode = GetWaitpid().GetExitCode();
#else
    HRESULT Status = S_OK;
    HPROCESS hProcess;
    DWORD dwExitCode = 0;
    if (SUCCEEDED(pProcess->GetHandle(&hProcess)))
    {
        GetExitCodeProcess(hProcess, &dwExitCode);
        exitCode = static_cast<int>(dwExitCode);
    }
#endif // FEATURE_PAL

    m_debugger.m_protocol->EmitExitedEvent(ExitedEvent(exitCode));
    m_debugger.NotifyProcessExited();
    m_debugger.m_protocol->EmitTerminatedEvent();
    m_debugger.m_ioredirect.async_cancel();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateThread(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_protocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitThread(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugThread *pThread)
{
    LogFuncEntry();
    ThreadId threadId(getThreadId(pThread));

    // TODO: clean evaluations and exceptions queues for current thread
    m_debugger.m_evaluator.NotifyEvalComplete(pThread, nullptr);
    if (m_debugger.GetLastStoppedThreadId() == threadId)
    {
        m_debugger.InvalidateLastStoppedThreadId();
    }
    m_debugger.m_protocol->EmitThreadEvent(ThreadEvent(ThreadExited, threadId));
    pAppDomain->Continue(0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadModule(
    /* [in] */ ICorDebugAppDomain *pAppDomain,
    /* [in] */ ICorDebugModule *pModule)
{
    LogFuncEntry();

    Module module;

    m_debugger.m_modules.TryLoadModuleSymbols(pModule, module, m_debugger.IsJustMyCode());
    m_debugger.m_protocol->EmitModuleEvent(ModuleEvent(ModuleNew, module));

    if (module.symbolStatus == SymbolsLoaded)
    {
        std::vector<BreakpointEvent> events;
        m_debugger.m_breakpoints.TryResolveBreakpointsForModule(pModule, events);
        for (const BreakpointEvent &event : events)
            m_debugger.m_protocol->EmitBreakpointEvent(event);
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
    if (m_debugger.m_evaluator.IsEvalRunning())
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

    m_debugger.m_protocol->EmitOutputEvent(OutputConsole, to_utf8(pMessage), src);
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
    ThreadId threadId(getThreadId(pThread));

    // In case we inside evaluation (exception during implicit function execution), make sure we continue process execution.
    // This is internal CoreCLR routine, should not be interrupted by debugger. CoreCLR will care about exception in this case
    // and provide exception data as evaluation result in case of unhandled exception.
    if (m_debugger.m_evaluator.IsEvalRunning() && m_debugger.m_evaluator.FindEvalForThread(pThread))
    {
        return pAppDomain->Continue(0);
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

    HRESULT Status = S_OK;
    std::string excType, excModule;
    IfFailRet(GetExceptionInfo(pThread, excType, excModule));

    ExceptionBreakMode mode;
    m_debugger.m_breakpoints.GetExceptionBreakMode(mode, "*");
    bool unhandled = (dwEventType == DEBUG_EXCEPTION_UNHANDLED && mode.Unhandled());
    bool not_matched = !(unhandled || m_debugger.MatchExceptionBreakpoint(dwEventType, excType, ExceptionBreakCategory::CLR));

    if (not_matched)
    {
        // TODO why we mixing debugger's output with user application output???
        std::string text = "Exception thrown: '" + excType + "' in " + excModule + "\n";
        m_debugger.m_protocol->EmitOutputEvent(OutputConsole, {&text[0], text.size()}, "target-exception");
        IfFailRet(pAppDomain->Continue(0));
        return S_OK;
    }

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
    IfFailRet(pThread->GetCurrentException(&pExceptionValue));
    IfFailRet(PrintStringField(pExceptionValue, fieldName, message));

    ToRelease<ICorDebugFrame> pActiveFrame;
    if (SUCCEEDED(pThread->GetActiveFrame(&pActiveFrame)) && pActiveFrame != nullptr)
        m_debugger.GetFrameLocation(pActiveFrame, threadId, FrameLevel(0), event.frame);

    m_debugger.SetLastStoppedThread(pThread);

    event.text = excType;
    event.description = message.empty() ? details : message;

    if (m_debugger.m_evaluator.IsEvalRunning() && !m_debugger.m_evaluator.is_empty_eval_queue())
    {
        ThreadId evalThreadId = m_debugger.m_evaluator.front_eval_queue();
        ToRelease<ICorDebugThread> pThreadEval;
        IfFailRet(m_debugger.m_pProcess->GetThread(int(evalThreadId), &pThreadEval));
        IfFailRet(pAppDomain->SetAllThreadsDebugState(THREAD_SUSPEND, nullptr));
        IfFailRet(pThreadEval->SetDebugState(THREAD_RUN));
        IfFailRet(pAppDomain->Continue(0));
        ToRelease<ICorDebugThread2> pThread2;
        IfFailRet(pThread->QueryInterface(IID_ICorDebugThread2, (LPVOID *)&pThread2));
        // Intercept exceptions from frame for resending. Its allow to avoid problem with
        // wrong state:"GS unsafe" and "optimized code" for evaluation of CallParametricFunc()
        IfFailRet(pThread2->InterceptCurrentException(pActiveFrame));
        return S_OK;
    }

    // TODO this looks like wrong logic with m_stopCounter, we must not have situation when process stop logic emit continued event.
    //      Exception breakpoint related code must be refactored.
    m_debugger.m_stopCounterMutex.lock();
    while (m_debugger.m_stopCounter > 0) {
        m_debugger.m_protocol->EmitContinuedEvent(m_debugger.GetLastStoppedThreadId());
        --m_debugger.m_stopCounter;
    }
    // INFO: Double EmitStopEvent() produce blocked coreclr command reader
    m_debugger.m_stopCounter = 1;
    m_debugger.m_stopCounterMutex.unlock();

    m_debugger.m_protocol->EmitStoppedEvent(event);
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

    HRESULT Status = S_OK;

    if (m_debugger.m_evaluator.IsEvalRunning())
    {
        // NOTE
        // All CoreCLR releases at least till version 3.1.3, don't have proper x86 implementation for ICorDebugEval::Abort().
        // This issue looks like CoreCLR terminate managed process execution instead of abort evaluation.

        ICorDebugEval *threadEval = m_debugger.m_evaluator.FindEvalForThread(pThread);
        if (threadEval != nullptr)
        {
            IfFailRet(threadEval->Abort());
        }
    }

    IfFailRet(pAppDomain->Continue(0));
    return S_OK;
}

} // namespace netcoredbg
