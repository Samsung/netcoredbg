// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "debugger/manageddebugger.h"

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
public:

    ULONG GetRefCount();

    ManagedCallback(ManagedDebugger &debugger) : m_refCount(0), m_debugger(debugger) {}

    // IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ICorDebugManagedCallback

    HRESULT STDMETHODCALLTYPE Breakpoint(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugBreakpoint *pBreakpoint) override;

    HRESULT STDMETHODCALLTYPE StepComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugStepper *pStepper,
        /* [in] */ CorDebugStepReason reason) override;

    HRESULT STDMETHODCALLTYPE Break(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *thread) override;

    HRESULT STDMETHODCALLTYPE Exception(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ BOOL unhandled) override;

    HRESULT STDMETHODCALLTYPE EvalComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugEval *pEval) override;

    HRESULT STDMETHODCALLTYPE EvalException(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugEval *pEval) override;

    HRESULT STDMETHODCALLTYPE CreateProcess(
        /* [in] */ ICorDebugProcess *pProcess) override;

    HRESULT STDMETHODCALLTYPE ExitProcess(
        /* [in] */ ICorDebugProcess *pProcess) override;

    HRESULT STDMETHODCALLTYPE CreateThread(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread) override;

    HRESULT STDMETHODCALLTYPE ExitThread(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread) override;

    HRESULT STDMETHODCALLTYPE LoadModule(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule) override;

    HRESULT STDMETHODCALLTYPE UnloadModule(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule) override;

    HRESULT STDMETHODCALLTYPE LoadClass(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugClass *c) override;

    HRESULT STDMETHODCALLTYPE UnloadClass(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugClass *c) override;

    HRESULT STDMETHODCALLTYPE DebuggerError(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ HRESULT errorHR,
        /* [in] */ DWORD errorCode) override;

    HRESULT STDMETHODCALLTYPE LogMessage(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ LONG lLevel,
        /* [in] */ WCHAR *pLogSwitchName,
        /* [in] */ WCHAR *pMessage) override;

    HRESULT STDMETHODCALLTYPE LogSwitch(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ LONG lLevel,
        /* [in] */ ULONG ulReason,
        /* [in] */ WCHAR *pLogSwitchName,
        /* [in] */ WCHAR *pParentName) override;

    HRESULT STDMETHODCALLTYPE CreateAppDomain(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ ICorDebugAppDomain *pAppDomain) override;

    HRESULT STDMETHODCALLTYPE ExitAppDomain(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ ICorDebugAppDomain *pAppDomain) override;

    HRESULT STDMETHODCALLTYPE LoadAssembly(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugAssembly *pAssembly) override;

    HRESULT STDMETHODCALLTYPE UnloadAssembly(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugAssembly *pAssembly) override;

    HRESULT STDMETHODCALLTYPE ControlCTrap(
        /* [in] */ ICorDebugProcess *pProcess) override;

    HRESULT STDMETHODCALLTYPE NameChange(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread) override;

    HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule,
        /* [in] */ IStream *pSymbolStream) override;

    HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pFunction,
        /* [in] */ BOOL fAccurate) override;

    HRESULT STDMETHODCALLTYPE BreakpointSetError(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugBreakpoint *pBreakpoint,
        /* [in] */ DWORD dwError) override;

    // ICorDebugManagedCallback2

    HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pOldFunction,
        /* [in] */ ICorDebugFunction *pNewFunction,
        /* [in] */ ULONG32 oldILOffset) override;

    HRESULT STDMETHODCALLTYPE CreateConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId,
        /* [in] */ WCHAR *pConnName) override;

    HRESULT STDMETHODCALLTYPE ChangeConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId) override;

    HRESULT STDMETHODCALLTYPE DestroyConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId) override;

    HRESULT STDMETHODCALLTYPE Exception(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFrame *pFrame,
        /* [in] */ ULONG32 nOffset,
        /* [in] */ CorDebugExceptionCallbackType dwEventType,
        /* [in] */ DWORD dwFlags) override;

    HRESULT STDMETHODCALLTYPE ExceptionUnwind(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
        /* [in] */ DWORD dwFlags) override;

    HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pFunction) override;

    HRESULT STDMETHODCALLTYPE MDANotification(
        /* [in] */ ICorDebugController *pController,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugMDA *pMDA) override;

    // ICorDebugManagedCallback3

    HRESULT STDMETHODCALLTYPE CustomNotification(
        /* [in] */ ICorDebugThread *pThread,  
        /* [in] */ ICorDebugAppDomain *pAppDomain) override;
};

} // namespace netcoredbg
