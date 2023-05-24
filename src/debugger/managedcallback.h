// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "debugger/manageddebugger.h"
#include <thread>
#include <list>

namespace netcoredbg
{

class ManagedCallback final : public ICorDebugManagedCallback, ICorDebugManagedCallback2, ICorDebugManagedCallback3
{
private:

    std::mutex m_refCountMutex;
    ULONG m_refCount;
    ManagedDebuggerHelpers &m_debugger;
    std::shared_ptr<CallbacksQueue> m_sharedCallbacksQueue;

public:

    ManagedCallback(ManagedDebuggerHelpers &debugger, std::shared_ptr<CallbacksQueue> &sharedCallbacksQueue) :
        m_refCount(0), m_debugger(debugger), m_sharedCallbacksQueue(sharedCallbacksQueue){}
    ULONG GetRefCount();

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
