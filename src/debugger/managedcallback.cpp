// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifdef _MSC_VER
#include <wtypes.h>
#endif

#include "debugger/managedcallback.h"
#include "debugger/callbacksqueue.h"
#include "debugger/threads.h"
#include "debugger/evalwaiter.h"
#include "debugger/breakpoints.h"
#include "debugger/waitpid.h"
#include "debugger/evalstackmachine.h"
#include "metadata/modules.h"
#include "interfaces/iprotocol.h"
#include "utils/utf.h"
#include "managed/interop.h"


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

HRESULT STDMETHODCALLTYPE ManagedCallback::Breakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        pBreakpoint->AddRef();
        m_sharedCallbacksQueue->EmplaceBack(CallbackQueueCall::Breakpoint, pAppDomain, pThread, pBreakpoint, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::StepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                        ICorDebugStepper *pStepper, CorDebugStepReason reason)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        m_sharedCallbacksQueue->EmplaceBack(CallbackQueueCall::StepComplete, pAppDomain, pThread, nullptr, reason, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Break(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->AddCallbackToQueue(pAppDomain, [&]()
    {
        pAppDomain->AddRef();
        pThread->AddRef();
        m_sharedCallbacksQueue->EmplaceBack(CallbackQueueCall::Break, pAppDomain, pThread, nullptr, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::Exception(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, BOOL unhandled)
{
    // Obsolete callback
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
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
// Remarks
// This method is not called until the common language runtime is initialized. Most of the ICorDebug methods will return CORDBG_E_NOTREADY before the CreateProcess callback.
HRESULT STDMETHODCALLTYPE ManagedCallback::CreateProcess(ICorDebugProcess *pProcess)
{
    LogFuncEntry();

    // ManagedPart must be initialized only once for process, since CoreCLR don't support unload and reinit
    // for global variables. coreclr_shutdown only should be called on process exit.
    Interop::Init(m_debugger.m_clrPath);

#ifdef INTEROP_DEBUGGING
    // Note, in case `attach` CoreCLR also call CreateProcess() that call this method.
    int error_n = 0;
    bool attach = m_debugger.m_startMethod == StartAttach;
    auto NotifyLastThreadExited = [&](int status)
    {
        // In case debuggee process rude terminated by some signal, we may have situation when
        // `ManagedCallback::ExitProcess()` will be newer called by dbgshim.
        if (!WIFSIGNALED(status))
            return;

        // If we still `Attached` here, `ManagedCallback::ExitProcess()` was not called.
        std::unique_lock<std::mutex> lockAttachedMutex(m_debugger.m_processAttachedMutex);
        if (m_debugger.m_processAttachedState == ProcessAttachedState::Attached)
            m_debugger.m_processAttachedCV.wait_for(lockAttachedMutex, std::chrono::milliseconds(3000));
        if (m_debugger.m_processAttachedState == ProcessAttachedState::Unattached)
            return;
        lockAttachedMutex.unlock();

        if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
            LOGW("The target process exited while evaluating the function.");

        m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(nullptr, nullptr);

        m_debugger.pProtocol->EmitExitedEvent(ExitedEvent(GetWaitpid().GetExitCode()));
        m_debugger.NotifyProcessExited();
        m_debugger.pProtocol->EmitTerminatedEvent();
        m_debugger.m_ioredirect.async_cancel();
    };
    if (m_debugger.m_interopDebugging &&
        FAILED(m_debugger.m_sharedInteropDebugger->Init((pid_t)m_debugger.m_processId, m_sharedCallbacksQueue, attach, NotifyLastThreadExited, error_n)))
    {
        LOGE("Interop debugging disabled due to initialization fail: %s", strerror(error_n));
        m_debugger.pProtocol->EmitInteropDebuggingErrorEvent(error_n);
        m_debugger.m_interopDebugging = false;
    }
#endif // INTEROP_DEBUGGING

    // Important! Care about callback queue before NotifyProcessCreated() call.
    // In case of `attach`, NotifyProcessCreated() call will notify debugger that debuggee process attached and debugger
    // should stop debuggee process by dirrect `Pause()` call. From another side, callback queue have bunch of asynchronous
    // added entries and, for example, `CreateThread()` could be called after this callback and broke our debugger logic.
    ToRelease<ICorDebugAppDomainEnum> domains;
    ICorDebugAppDomain *pAppDomain;
    ULONG domainsFetched;
    if (SUCCEEDED(pProcess->EnumerateAppDomains(&domains)))
    {
        // At this point we have only one domain for sure.
        if (SUCCEEDED(domains->Next(1, &pAppDomain, &domainsFetched)) && domainsFetched == 1)
        {
            // Don't AddRef() here for pAppDomain! We get it with AddRef() from Next() and will release in m_callbacksQueue by ToRelease destructor.
            return m_sharedCallbacksQueue->AddCallbackToQueue(pAppDomain, [&]()
            {
                m_sharedCallbacksQueue->EmplaceBack(CallbackQueueCall::CreateProcess, pAppDomain, nullptr, nullptr, STEP_NORMAL, ExceptionCallbackType::FIRST_CHANCE);
            });
        }
    }

    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
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

    m_debugger.pProtocol->EmitExitedEvent(ExitedEvent(exitCode));
    m_debugger.NotifyProcessExited();
    m_debugger.pProtocol->EmitTerminatedEvent();
    m_debugger.m_ioredirect.async_cancel();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();

    if (m_debugger.m_sharedEvalWaiter->IsEvalRunning())
        LOGW("Thread was created by user code during evaluation with implicit user code execution.");

    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Add(threadId, m_debugger.m_startMethod == StartAttach);

    m_debugger.pProtocol->EmitThreadEvent(ThreadEvent(ManagedThreadStarted, threadId, m_debugger.m_interopDebugging));
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();

    ThreadId threadId(getThreadId(pThread));
    m_debugger.m_sharedThreads->Remove(threadId);

    m_debugger.m_sharedEvalWaiter->NotifyEvalComplete(pThread, nullptr);
    if (m_debugger.GetLastStoppedThreadId() == threadId)
        m_debugger.InvalidateLastStoppedThreadId();

    m_debugger.m_sharedBreakpoints->ManagedCallbackExitThread(pThread);

    m_debugger.pProtocol->EmitThreadEvent(ThreadEvent(ManagedThreadExited, threadId, m_debugger.m_interopDebugging));
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule)
{
    LogFuncEntry();

    Module module;
    std::string outputText;
    m_debugger.m_sharedModules->TryLoadModuleSymbols(pModule, module, m_debugger.IsJustMyCode(), m_debugger.IsHotReload(), outputText);
    if (!outputText.empty())
    {
        m_debugger.pProtocol->EmitOutputEvent(OutputStdErr, outputText);
    }
    m_debugger.pProtocol->EmitModuleEvent(ModuleEvent(ModuleNew, module));

    if (module.symbolStatus == SymbolsLoaded)
    {
        std::vector<BreakpointEvent> events;
        m_debugger.m_sharedBreakpoints->ManagedCallbackLoadModule(pModule, events);
        for (const BreakpointEvent &event : events)
        {
            m_debugger.pProtocol->EmitBreakpointEvent(event);
        }
    }
    m_debugger.m_sharedBreakpoints->ManagedCallbackLoadModuleAll(pModule);

    // enable Debugger.NotifyOfCrossThreadDependency after System.Private.CoreLib.dll loaded (trigger for 1 time call only)
    if (module.name == "System.Private.CoreLib.dll")
    {
        m_debugger.m_sharedEvalWaiter->SetupCrossThreadDependencyNotificationClass(pModule);
        m_debugger.m_sharedEvalStackMachine->FindPredefinedTypes(pModule);
    }

    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DebuggerError(ICorDebugProcess *pProcess, HRESULT errorHR, DWORD errorCode)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
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

    DWORD threadId = 0;
    pThread->GetID(&threadId);
    m_debugger.pProtocol->EmitOutputEvent(OutputStdOut, to_utf8(pMessage), src, threadId);
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LogSwitch(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, LONG lLevel,
                                                     ULONG ulReason, WCHAR *pLogSwitchName, WCHAR *pParentName)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExitAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::LoadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UnloadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ControlCTrap(ICorDebugProcess *pProcess)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::NameChange(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::UpdateModuleSymbols(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule, IStream *pSymbolStream)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::EditAndContinueRemap(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                                ICorDebugFunction *pFunction, BOOL fAccurate)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::BreakpointSetError(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                              ICorDebugBreakpoint *pBreakpoint, DWORD dwError)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}


// ICorDebugManagedCallback2

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapOpportunity(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                                    ICorDebugFunction *pOldFunction, ICorDebugFunction *pNewFunction, ULONG32 oldILOffset)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::CreateConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId, WCHAR *pConnName)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ChangeConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::DestroyConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueProcess(pProcess);
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
    return m_sharedCallbacksQueue->AddCallbackToQueue(pAppDomain, [&]()
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
        m_sharedCallbacksQueue->EmplaceBack(CallbackQueueCall::Exception, pAppDomain, pThread, nullptr, STEP_NORMAL, eventType, excModule);
    });
}

HRESULT STDMETHODCALLTYPE ManagedCallback::ExceptionUnwind(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                           CorDebugExceptionUnwindCallbackType dwEventType, DWORD dwFlags)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::FunctionRemapComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugFunction *pFunction)
{
    LogFuncEntry();
    return m_sharedCallbacksQueue->ContinueAppDomain(pAppDomain);
}

HRESULT STDMETHODCALLTYPE ManagedCallback::MDANotification(ICorDebugController *pController, ICorDebugThread *pThread, ICorDebugMDA *pMDA)
{
    LogFuncEntry();
    ToRelease<ICorDebugProcess> iCorProcess;
    pThread->GetProcess(&iCorProcess);
    return m_sharedCallbacksQueue->ContinueProcess(iCorProcess);
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
