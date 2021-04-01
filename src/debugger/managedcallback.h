// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "debugger/manageddebugger.h"

namespace netcoredbg
{

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2, ICorDebugManagedCallback3
{
    std::mutex m_refCountMutex;
    ULONG m_refCount;
    ManagedDebugger &m_debugger;
public:

    void HandleEvent(ICorDebugController *controller, const std::string &eventName);
    ULONG GetRefCount();

    ManagedCallback(ManagedDebugger &debugger) : m_refCount(0), m_debugger(debugger) {}
    virtual ~ManagedCallback() {}

    // IUnknown

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    // ICorDebugManagedCallback

    virtual HRESULT STDMETHODCALLTYPE Breakpoint(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugBreakpoint *pBreakpoint);

    virtual HRESULT STDMETHODCALLTYPE StepComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugStepper *pStepper,
        /* [in] */ CorDebugStepReason reason);

    virtual HRESULT STDMETHODCALLTYPE Break(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *thread);

    virtual HRESULT STDMETHODCALLTYPE Exception(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ BOOL unhandled);

    virtual HRESULT STDMETHODCALLTYPE EvalComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugEval *pEval);

    virtual HRESULT STDMETHODCALLTYPE EvalException(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugEval *pEval);

    virtual HRESULT STDMETHODCALLTYPE CreateProcess(
        /* [in] */ ICorDebugProcess *pProcess);

    virtual HRESULT STDMETHODCALLTYPE ExitProcess(
        /* [in] */ ICorDebugProcess *pProcess);

    virtual HRESULT STDMETHODCALLTYPE CreateThread(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread);

    virtual HRESULT STDMETHODCALLTYPE ExitThread(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread);

    virtual HRESULT STDMETHODCALLTYPE LoadModule(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule);

    virtual HRESULT STDMETHODCALLTYPE UnloadModule(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule);

    virtual HRESULT STDMETHODCALLTYPE LoadClass(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugClass *c);

    virtual HRESULT STDMETHODCALLTYPE UnloadClass(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugClass *c);

    virtual HRESULT STDMETHODCALLTYPE DebuggerError(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ HRESULT errorHR,
        /* [in] */ DWORD errorCode);

    virtual HRESULT STDMETHODCALLTYPE LogMessage(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ LONG lLevel,
        /* [in] */ WCHAR *pLogSwitchName,
        /* [in] */ WCHAR *pMessage);

    virtual HRESULT STDMETHODCALLTYPE LogSwitch(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ LONG lLevel,
        /* [in] */ ULONG ulReason,
        /* [in] */ WCHAR *pLogSwitchName,
        /* [in] */ WCHAR *pParentName);

    virtual HRESULT STDMETHODCALLTYPE CreateAppDomain(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ ICorDebugAppDomain *pAppDomain);

    virtual HRESULT STDMETHODCALLTYPE ExitAppDomain(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ ICorDebugAppDomain *pAppDomain);

    virtual HRESULT STDMETHODCALLTYPE LoadAssembly(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugAssembly *pAssembly);

    virtual HRESULT STDMETHODCALLTYPE UnloadAssembly(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugAssembly *pAssembly);

    virtual HRESULT STDMETHODCALLTYPE ControlCTrap(
        /* [in] */ ICorDebugProcess *pProcess);

    virtual HRESULT STDMETHODCALLTYPE NameChange(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread);

    virtual HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugModule *pModule,
        /* [in] */ IStream *pSymbolStream);

    virtual HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pFunction,
        /* [in] */ BOOL fAccurate);

    virtual HRESULT STDMETHODCALLTYPE BreakpointSetError(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugBreakpoint *pBreakpoint,
        /* [in] */ DWORD dwError);

    // ICorDebugManagedCallback2

    virtual HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pOldFunction,
        /* [in] */ ICorDebugFunction *pNewFunction,
        /* [in] */ ULONG32 oldILOffset);

    virtual HRESULT STDMETHODCALLTYPE CreateConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId,
        /* [in] */ WCHAR *pConnName);

    virtual HRESULT STDMETHODCALLTYPE ChangeConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId);

    virtual HRESULT STDMETHODCALLTYPE DestroyConnection(
        /* [in] */ ICorDebugProcess *pProcess,
        /* [in] */ CONNID dwConnectionId);

    virtual HRESULT STDMETHODCALLTYPE Exception(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFrame *pFrame,
        /* [in] */ ULONG32 nOffset,
        /* [in] */ CorDebugExceptionCallbackType dwEventType,
        /* [in] */ DWORD dwFlags);

    virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
        /* [in] */ DWORD dwFlags);

    virtual HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
        /* [in] */ ICorDebugAppDomain *pAppDomain,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugFunction *pFunction);

    virtual HRESULT STDMETHODCALLTYPE MDANotification(
        /* [in] */ ICorDebugController *pController,
        /* [in] */ ICorDebugThread *pThread,
        /* [in] */ ICorDebugMDA *pMDA);

    // ICorDebugManagedCallback3

    virtual HRESULT STDMETHODCALLTYPE CustomNotification(
        /* [in] */ ICorDebugThread *pThread,  
        /* [in] */ ICorDebugAppDomain *pAppDomain);
};

} // namespace netcoredbg
