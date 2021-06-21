// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "debugger/manageddebugger.h"
#include <thread>
#include <list>

namespace netcoredbg
{

// https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/icordebugcontroller-hasqueuedcallbacks-method
//
// Callbacks will be dispatched one at a time, each time ICorDebugController::Continue is called.
// The debugger can check this flag if it wants to report multiple debugging events that occur simultaneously.
// 
// When debugging events are queued, they have already occurred, so the debugger must drain the entire queue
// to be sure of the state of the debuggee. (Call ICorDebugController::Continue to drain the queue.) For example,
// if the queue contains two debugging events on thread X, and the debugger suspends thread X after the first debugging
// event and then calls ICorDebugController::Continue, the second debugging event for thread X will be dispatched although
// the thread has been suspended.

class ManagedCallback final : public ICorDebugManagedCallback, ICorDebugManagedCallback2, ICorDebugManagedCallback3
{
    std::mutex m_refCountMutex;
    ULONG m_refCount;
    ManagedDebugger &m_debugger;

    enum class CallbackQueueCall
    {
        FinishWorker = 0,
        Breakpoint,
        StepComplete,
        Break,
        Exception
    };

    struct CallbackQueueEntry
    {
        CallbackQueueCall Call;
        ToRelease<ICorDebugAppDomain> iCorAppDomain;
        ToRelease<ICorDebugThread> iCorThread;
        ToRelease<ICorDebugBreakpoint> iCorBreakpoint;
        CorDebugStepReason Reason;
        ExceptionCallbackType EventType;
        std::string ExcModule;

        CallbackQueueEntry(CallbackQueueCall call,
                           ICorDebugAppDomain *pAppDomain,
                           ICorDebugThread *pThread,
                           ICorDebugBreakpoint *pBreakpoint,
                           CorDebugStepReason reason,
                           ExceptionCallbackType eventType,
                           const std::string &excModule = "") :
            Call(call),
            iCorAppDomain(pAppDomain),
            iCorThread(pThread),
            iCorBreakpoint(pBreakpoint),
            Reason(reason),
            EventType(eventType),
            ExcModule(excModule)
        {}
    };

    std::mutex m_callbacksMutex;
    std::condition_variable m_callbacksCV;
    std::list<CallbackQueueEntry> m_callbacksQueue; // Make sure this one initialized before m_callbacksWorker.
    bool m_stopEventInProcess; // Make sure this one initialized before m_callbacksWorker.
    std::thread m_callbacksWorker;

    void CallbacksWorker();
    bool CallbacksWorkerBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);
    bool CallbacksWorkerStepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, CorDebugStepReason reason);
    bool CallbacksWorkerBreak(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread);
    bool CallbacksWorkerException(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ExceptionCallbackType eventType, const std::string &excModule);
    HRESULT AddCallbackToQueue(ICorDebugAppDomain *pAppDomain, std::function<void()> callback);
    bool HasQueuedCallbacks(ICorDebugProcess *pProcess);
    HRESULT ContinueAppDomainWithCallbacksQueue(ICorDebugAppDomain *pAppDomain);
    HRESULT ContinueProcessWithCallbacksQueue(ICorDebugProcess *pProcess);

public:

    ManagedCallback(ManagedDebugger &debugger) :
        m_refCount(0), m_debugger(debugger), m_stopEventInProcess(false), m_callbacksWorker{&ManagedCallback::CallbacksWorker, this} {}
    ~ManagedCallback();
    ULONG GetRefCount();

    // Called from ManagedDebugger by protocol request (Continue/StepCommand/Pause).
    bool IsRunning();
    HRESULT Continue(ICorDebugProcess *pProcess);
    HRESULT Pause(ICorDebugProcess *pProcess);

    // IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ICorDebugManagedCallback

    HRESULT STDMETHODCALLTYPE Breakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint) override;
    HRESULT STDMETHODCALLTYPE StepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                           ICorDebugStepper *pStepper, CorDebugStepReason reason) override;
    HRESULT STDMETHODCALLTYPE Break(ICorDebugAppDomain *pAppDomain, ICorDebugThread *thread) override;
    HRESULT STDMETHODCALLTYPE Exception(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, BOOL unhandled) override;
    HRESULT STDMETHODCALLTYPE EvalComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugEval *pEval) override;
    HRESULT STDMETHODCALLTYPE EvalException(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugEval *pEval) override;
    HRESULT STDMETHODCALLTYPE CreateProcess(ICorDebugProcess *pProcess) override;
    HRESULT STDMETHODCALLTYPE ExitProcess(ICorDebugProcess *pProcess) override;
    HRESULT STDMETHODCALLTYPE CreateThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread) override;
    HRESULT STDMETHODCALLTYPE ExitThread(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread) override;
    HRESULT STDMETHODCALLTYPE LoadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule) override;
    HRESULT STDMETHODCALLTYPE UnloadModule(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule) override;
    HRESULT STDMETHODCALLTYPE LoadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c) override;
    HRESULT STDMETHODCALLTYPE UnloadClass(ICorDebugAppDomain *pAppDomain, ICorDebugClass *c) override;
    HRESULT STDMETHODCALLTYPE DebuggerError(ICorDebugProcess *pProcess, HRESULT errorHR, DWORD errorCode) override;
    HRESULT STDMETHODCALLTYPE LogMessage(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, LONG lLevel,
                                         WCHAR *pLogSwitchName, WCHAR *pMessage) override;
    HRESULT STDMETHODCALLTYPE LogSwitch(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, LONG lLevel,
                                        ULONG ulReason, WCHAR *pLogSwitchName, WCHAR *pParentName) override;
    HRESULT STDMETHODCALLTYPE CreateAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain) override;
    HRESULT STDMETHODCALLTYPE ExitAppDomain(ICorDebugProcess *pProcess, ICorDebugAppDomain *pAppDomain) override;
    HRESULT STDMETHODCALLTYPE LoadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly) override;
    HRESULT STDMETHODCALLTYPE UnloadAssembly(ICorDebugAppDomain *pAppDomain, ICorDebugAssembly *pAssembly) override;
    HRESULT STDMETHODCALLTYPE ControlCTrap(ICorDebugProcess *pProcess) override;
    HRESULT STDMETHODCALLTYPE NameChange(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread) override;
    HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(ICorDebugAppDomain *pAppDomain, ICorDebugModule *pModule, IStream *pSymbolStream) override;
    HRESULT STDMETHODCALLTYPE EditAndContinueRemap(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                   ICorDebugFunction *pFunction, BOOL fAccurate) override;
    HRESULT STDMETHODCALLTYPE BreakpointSetError(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                 ICorDebugBreakpoint *pBreakpoint, DWORD dwError) override;

    // ICorDebugManagedCallback2

    HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                                       ICorDebugFunction *pOldFunction, ICorDebugFunction *pNewFunction, ULONG32 oldILOffset) override;
    HRESULT STDMETHODCALLTYPE CreateConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId, WCHAR *pConnName) override;
    HRESULT STDMETHODCALLTYPE ChangeConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId) override;
    HRESULT STDMETHODCALLTYPE DestroyConnection(ICorDebugProcess *pProcess, CONNID dwConnectionId) override;
    HRESULT STDMETHODCALLTYPE Exception(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugFrame *pFrame,
                                        ULONG32 nOffset, CorDebugExceptionCallbackType dwEventType, DWORD dwFlags) override;
    HRESULT STDMETHODCALLTYPE ExceptionUnwind(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread,
                                              CorDebugExceptionUnwindCallbackType dwEventType, DWORD dwFlags) override;
    HRESULT STDMETHODCALLTYPE FunctionRemapComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugFunction *pFunction) override;
    HRESULT STDMETHODCALLTYPE MDANotification(ICorDebugController *pController, ICorDebugThread *pThread, ICorDebugMDA *pMDA) override;

    // ICorDebugManagedCallback3

    HRESULT STDMETHODCALLTYPE CustomNotification(ICorDebugThread *pThread, ICorDebugAppDomain *pAppDomain) override;
};

} // namespace netcoredbg
