// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <vector>
#include <list>
#include <chrono>

#include "symbolreader.h"
#include "cputil.h"
#include "platform.h"
#include "typeprinter.h"
#include "debugger.h"
#include "frames.h"

#define __in
#define __out
#include "dbgshim.h"
#undef __in
#undef __out


void Debugger::NotifyProcessCreated()
{
    std::lock_guard<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessAttached;
}

void Debugger::NotifyProcessExited()
{
    std::lock_guard<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessUnattached;
    m_processAttachedMutex.unlock();
    m_processAttachedCV.notify_one();
}

void Debugger::WaitProcessExited()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    if (m_processAttachedState != ProcessUnattached)
        m_processAttachedCV.wait(lock, [this]{return m_processAttachedState == ProcessUnattached;});
}

size_t NextOSPageAddress (size_t addr)
{
    size_t pageSize = OSPageSize();
    return (addr+pageSize)&(~(pageSize-1));
}

/**********************************************************************\
* Routine Description:                                                 *
*                                                                      *
*    This function is called to read memory from the debugee's         *
*    address space.  If the initial read fails, it attempts to read    *
*    only up to the edge of the page containing "offset".              *
*                                                                      *
\**********************************************************************/
BOOL SafeReadMemory (TADDR offset, PVOID lpBuffer, ULONG cb,
                     PULONG lpcbBytesRead)
{
    return FALSE;
    // TODO: In-memory PDB?
    // std::lock_guard<std::mutex> lock(g_processMutex);

    // if (!g_process)
    //     return FALSE;

    // BOOL bRet = FALSE;

    // SIZE_T bytesRead = 0;

    // bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
    //                                        &bytesRead));

    // if (!bRet)
    // {
    //     cb   = (ULONG)(NextOSPageAddress(offset) - offset);
    //     bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
    //                                         &bytesRead));
    // }

    // *lpcbBytesRead = bytesRead;
    // return bRet;
}

std::mutex MIProtocol::m_outMutex;

void MIProtocol::Printf(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(m_outMutex);
    va_list arg;

    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);

    fflush(stdout);
}

std::string MIProtocol::EscapeMIValue(const std::string &str)
{
    std::string s(str);

    for (std::size_t i = 0; i < s.size(); ++i)
    {
        int count = 0;
        char c = s.at(i);
        switch (c)
        {
            case '\"': count = 1; s.insert(i, count, '\\'); s[i + count] = '\"'; break;
            case '\\': count = 1; s.insert(i, count, '\\'); s[i + count] = '\\'; break;
            case '\0': count = 1; s.insert(i, count, '\\'); s[i + count] = '0'; break;
            case '\a': count = 1; s.insert(i, count, '\\'); s[i + count] = 'a'; break;
            case '\b': count = 1; s.insert(i, count, '\\'); s[i + count] = 'b'; break;
            case '\f': count = 1; s.insert(i, count, '\\'); s[i + count] = 'f'; break;
            case '\n': count = 1; s.insert(i, count, '\\'); s[i + count] = 'n'; break;
            case '\r': count = 1; s.insert(i, count, '\\'); s[i + count] = 'r'; break;
            case '\t': count = 1; s.insert(i, count, '\\'); s[i + count] = 't'; break;
            case '\v': count = 1; s.insert(i, count, '\\'); s[i + count] = 'v'; break;
        }
        i += count;
    }

    return s;
}

static HRESULT DisableAllSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;
    ToRelease<ICorDebugStepperEnum> steppers;
    IfFailRet(pAppDomain->EnumerateSteppers(&steppers));

    ICorDebugStepper *curStepper;
    ULONG steppersFetched;
    while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
    {
        ToRelease<ICorDebugStepper> pStepper(curStepper);
        pStepper->Deactivate();
    }

    return S_OK;
}

HRESULT Debugger::DisableAllSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

static HRESULT DisableAllBreakpointsAndSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;

    ToRelease<ICorDebugBreakpointEnum> breakpoints;
    if (SUCCEEDED(pAppDomain->EnumerateBreakpoints(&breakpoints)))
    {
        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
            pBreakpoint->Activate(FALSE);
        }
    }

    DisableAllSteppersInAppDomain(pAppDomain);

    return S_OK;
}

HRESULT DisableAllBreakpointsAndSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllBreakpointsAndSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

void Debugger::SetLastStoppedThread(ICorDebugThread *pThread)
{
    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    m_lastStoppedThreadId = threadId;
}

int Debugger::GetLastStoppedThreadId()
{
    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    return m_lastStoppedThreadId;
}

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

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2
{
    friend class Debugger;
    ULONG m_refCount;
    Debugger *m_debugger;
public:

        void HandleEvent(ICorDebugController *controller, const std::string &eventName)
        {
            std::string text = "Event received: '" + eventName + "'\n";
            m_debugger->m_protocol->EmitOutputEvent(OutputEvent(OutputConsole, text));
            controller->Continue(0);
        }

        ManagedCallback() : m_refCount(1), m_debugger(nullptr) {}
        virtual ~ManagedCallback() {}

        // IUnknown

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface)
        {
            if(riid == __uuidof(ICorDebugManagedCallback))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(ICorDebugManagedCallback2))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback2*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(IUnknown))
            {
                *ppInterface = static_cast<IUnknown*>(static_cast<ICorDebugManagedCallback*>(this));
                AddRef();
                return S_OK;
            }
            else
            {
                return E_NOINTERFACE;
            }
        }

        virtual ULONG STDMETHODCALLTYPE AddRef()
        {
            return InterlockedIncrement((volatile LONG *) &m_refCount);
        }

        virtual ULONG STDMETHODCALLTYPE Release()
        {
            ULONG count = InterlockedDecrement((volatile LONG *) &m_refCount);
            if(count == 0)
            {
                delete this;
            }
            return count;
        }

        // ICorDebugManagedCallback

        virtual HRESULT STDMETHODCALLTYPE Breakpoint(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint)
        {
            if (m_debugger->m_evaluator.IsEvalRunning())
            {
                pAppDomain->Continue(0);
                return S_OK;
            }

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            StoppedEvent event(StopBreakpoint, threadId);
            m_debugger->HitBreakpoint(pThread, event.breakpoint);

            ToRelease<ICorDebugFrame> pFrame;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                m_debugger->GetFrameLocation(pFrame, threadId, 0, event.frame);

            m_debugger->SetLastStoppedThread(pThread);
            m_debugger->m_protocol->EmitStoppedEvent(event);

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE StepComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugStepper *pStepper,
            /* [in] */ CorDebugStepReason reason)
        {
            DWORD threadId = 0;
            pThread->GetID(&threadId);

            StackFrame stackFrame;
            ToRelease<ICorDebugFrame> pFrame;
            HRESULT Status = S_FALSE;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                Status = m_debugger->GetFrameLocation(pFrame, threadId, 0, stackFrame);

            const bool no_source = Status == S_FALSE;

            if (m_debugger->IsJustMyCode() && no_source)
            {
                m_debugger->SetupStep(pThread, Debugger::STEP_OVER);
                pAppDomain->Continue(0);
            }
            else
            {
                StoppedEvent event(StopStep, threadId);
                event.frame = stackFrame;

                m_debugger->SetLastStoppedThread(pThread);
                m_debugger->m_protocol->EmitStoppedEvent(event);
            }
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread) { HandleEvent(pAppDomain, "Break"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ BOOL unhandled)
        {
            std::string excType;
            std::string excModule;
            GetExceptionInfo(pThread, excType, excModule);

            if (unhandled)
            {
                DWORD threadId = 0;
                pThread->GetID(&threadId);

                StackFrame stackFrame;
                ToRelease<ICorDebugFrame> pFrame;
                if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                    m_debugger->GetFrameLocation(pFrame, threadId, 0, stackFrame);

                m_debugger->SetLastStoppedThread(pThread);

                std::string details = "An unhandled exception of type '" + excType + "' occurred in " + excModule;

                ToRelease<ICorDebugValue> pExceptionValue;

                auto emitFunc = [=](const std::string &message) {
                    StoppedEvent event(StopException, threadId);
                    event.text = excType;
                    event.description = message.empty() ? details : message;
                    event.frame = stackFrame;
                    m_debugger->m_protocol->EmitStoppedEvent(event);
                };

                if (FAILED(pThread->GetCurrentException(&pExceptionValue)) ||
                    FAILED(m_debugger->m_evaluator.ObjectToString(pThread, pExceptionValue, emitFunc)))
                {
                    emitFunc(details);
                }
            }
            else
            {
                std::string text = "Exception thrown: '" + excType + "' in " + excModule + "\n";
                OutputEvent event(OutputConsole, text);
                event.source = "target-exception";
                m_debugger->m_protocol->EmitOutputEvent(event);
                pAppDomain->Continue(0);
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            m_debugger->m_evaluator.NotifyEvalComplete(pThread, pEval);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalException(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            m_debugger->m_evaluator.NotifyEvalComplete(pThread, pEval);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            //HandleEvent(pProcess, "CreateProcess");
            m_debugger->NotifyProcessCreated();
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            m_debugger->m_evaluator.NotifyEvalComplete(nullptr, nullptr);
            m_debugger->m_protocol->EmitExitedEvent(ExitedEvent(0));
            m_debugger->NotifyProcessExited();
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            DWORD threadId = 0;
            thread->GetID(&threadId);
            m_debugger->m_protocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            m_debugger->m_evaluator.NotifyEvalComplete(thread, nullptr);
            DWORD threadId = 0;
            thread->GetID(&threadId);
            m_debugger->m_protocol->EmitThreadEvent(ThreadEvent(ThreadExited, threadId));
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule)
        {
            std::string id;
            std::string name;
            bool symbolsLoaded = false;
            CORDB_ADDRESS baseAddress = 0;
            ULONG32 size = 0;

            m_debugger->m_modules.TryLoadModuleSymbols(pModule, id, name, symbolsLoaded, baseAddress, size);

            std::stringstream ss;
            ss << "id=\"{" << id << "}\","
                << "target-name=\"" << MIProtocol::EscapeMIValue(name) << "\","
                << "host-name=\"" << MIProtocol::EscapeMIValue(name) << "\","
                << "symbols-loaded=\"" << symbolsLoaded << "\","
                << "base-address=\"0x" << std::hex << baseAddress << "\","
                << "size=\"" << std::dec << size << "\"";
            MIProtocol::Printf("=library-loaded,%s\n", ss.str().c_str());

            if (symbolsLoaded)
                m_debugger->TryResolveBreakpointsForModule(pModule);

            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule) { HandleEvent(pAppDomain, "UnloadModule"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "LoadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE UnloadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "UnloadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DebuggerError(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ HRESULT errorHR,
            /* [in] */ DWORD errorCode) { printf("DebuggerError\n"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogMessage(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pMessage) { pAppDomain->Continue(0); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogSwitch(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ ULONG ulReason,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pParentName) { pAppDomain->Continue(0); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain)
        {
            //HandleEvent(pProcess, "CreateAppDomain");
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain) { HandleEvent(pAppDomain, "ExitAppDomain"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly)
        {
            //HandleEvent(pAppDomain, "LoadAssembly");
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ControlCTrap(
            /* [in] */ ICorDebugProcess *pProcess) { HandleEvent(pProcess, "ControlCTrap"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE NameChange(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread)
        {
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule,
            /* [in] */ IStream *pSymbolStream) { HandleEvent(pAppDomain, "UpdateModuleSymbols"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction,
            /* [in] */ BOOL fAccurate) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE BreakpointSetError(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint,
            /* [in] */ DWORD dwError) {return S_OK; }


        // ICorDebugManagedCallback2

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pOldFunction,
            /* [in] */ ICorDebugFunction *pNewFunction,
            /* [in] */ ULONG32 oldILOffset) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId,
            /* [in] */ WCHAR *pConnName) { HandleEvent(pProcess, "CreateConnection"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ChangeConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) { HandleEvent(pProcess, "ChangeConnection"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DestroyConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFrame *pFrame,
            /* [in] */ ULONG32 nOffset,
            /* [in] */ CorDebugExceptionCallbackType dwEventType,
            /* [in] */ DWORD dwFlags)
        {
          // const char *cbTypeName;
          // switch(dwEventType)
          // {
          //     case DEBUG_EXCEPTION_FIRST_CHANCE: cbTypeName = "FIRST_CHANCE"; break;
          //     case DEBUG_EXCEPTION_USER_FIRST_CHANCE: cbTypeName = "USER_FIRST_CHANCE"; break;
          //     case DEBUG_EXCEPTION_CATCH_HANDLER_FOUND: cbTypeName = "CATCH_HANDLER_FOUND"; break;
          //     case DEBUG_EXCEPTION_UNHANDLED: cbTypeName = "UNHANDLED"; break;
          //     default: cbTypeName = "?"; break;
          // }
          // Debugger::Printf("*stopped,reason=\"exception-received2\",exception-stage=\"%s\"\n",
          //     cbTypeName);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
            /* [in] */ DWORD dwFlags) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE MDANotification(
            /* [in] */ ICorDebugController *pController,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugMDA *pMDA) { HandleEvent(pController, "MDANotification"); return S_OK; }
};

Debugger::Debugger() :
    m_processAttachedState(ProcessUnattached),
    m_evaluator(m_modules),
    m_managedCallback(new ManagedCallback()),
    m_pDebug(nullptr),
    m_pProcess(nullptr),
    m_justMyCode(true),
    m_startupReady(false),
    m_startupResult(S_OK),
    m_unregisterToken(nullptr),
    m_processId(0),
    m_nextVariableReference(1),
    m_nextBreakpointId(1)
{
    m_managedCallback->m_debugger = this;
}

Debugger::~Debugger()
{
    m_managedCallback->Release();
}

VOID Debugger::StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr)
{
    Debugger *self = static_cast<Debugger*>(parameter);

    std::unique_lock<std::mutex> lock(self->m_startupMutex);

    self->m_startupResult = FAILED(hr) ? hr : self->Startup(pCordb, self->m_processId);
    self->m_startupReady = true;

    if (self->m_unregisterToken)
    {
        UnregisterForRuntimeStartup(self->m_unregisterToken);
        self->m_unregisterToken = nullptr;
    }

    lock.unlock();
    self->m_startupCV.notify_one();
}

// From dbgshim.cpp
static bool AreAllHandlesValid(HANDLE *handleArray, DWORD arrayLength)
{
    for (DWORD i = 0; i < arrayLength; i++)
    {
        HANDLE h = handleArray[i];
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
    }
    return true;
}

static HRESULT InternalEnumerateCLRs(int pid, HANDLE **ppHandleArray, LPWSTR **ppStringArray, DWORD *pdwArrayLength)
{
    int numTries = 0;
    HRESULT hr;

    while (numTries < 25)
    {
        hr = EnumerateCLRs(pid, ppHandleArray, ppStringArray, pdwArrayLength);

        // EnumerateCLRs uses the OS API CreateToolhelp32Snapshot which can return ERROR_BAD_LENGTH or
        // ERROR_PARTIAL_COPY. If we get either of those, we try wait 1/10th of a second try again (that
        // is the recommendation of the OS API owners).
        if ((hr != HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY)) && (hr != HRESULT_FROM_WIN32(ERROR_BAD_LENGTH)))
        {
            // Just return any other error or if no handles were found (which means the coreclr module wasn't found yet).
            if (FAILED(hr) || *ppHandleArray == NULL || *pdwArrayLength <= 0)
            {
                return hr;
            }
            // If EnumerateCLRs succeeded but any of the handles are INVALID_HANDLE_VALUE, then sleep and retry
            // also. This fixes a race condition where dbgshim catches the coreclr module just being loaded but
            // before g_hContinueStartupEvent has been initialized.
            if (AreAllHandlesValid(*ppHandleArray, *pdwArrayLength))
            {
                return hr;
            }
            // Clean up memory allocated in EnumerateCLRs since this path it succeeded
            CloseCLREnumeration(*ppHandleArray, *ppStringArray, *pdwArrayLength);

            *ppHandleArray = NULL;
            *ppStringArray = NULL;
            *pdwArrayLength = 0;
        }

        // Sleep and retry enumerating the runtimes
        Sleep(100);
        numTries++;

        // if (m_canceled)
        // {
        //     break;
        // }
    }

    // Indicate a timeout
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    return hr;
}

static std::string GetCLRPath(int pid)
{
    HANDLE* pHandleArray;
    LPWSTR* pStringArray;
    DWORD dwArrayLength;
    if (FAILED(InternalEnumerateCLRs(pid, &pHandleArray, &pStringArray, &dwArrayLength)) || dwArrayLength == 0)
        return std::string();

    std::string result = to_utf8(pStringArray[0]);

    CloseCLREnumeration(pHandleArray, pStringArray, dwArrayLength);

    return result;
}

HRESULT Debugger::Startup(IUnknown *punk, int pid)
{
    HRESULT Status;

    Cleanup();

    ToRelease<ICorDebug> pCorDebug;
    IfFailRet(punk->QueryInterface(IID_ICorDebug, (void **)&pCorDebug));

    IfFailRet(pCorDebug->Initialize());

    Status = pCorDebug->SetManagedHandler(m_managedCallback);
    if (FAILED(Status))
    {
        pCorDebug->Terminate();
        return Status;
    }

    if (m_clrPath.empty())
        m_clrPath = GetCLRPath(pid);

    SymbolReader::SetCoreCLRPath(m_clrPath);

    ToRelease<ICorDebugProcess> pProcess;
    Status = pCorDebug->DebugActiveProcess(pid, FALSE, &pProcess);
    if (FAILED(Status))
    {
        pCorDebug->Terminate();
        return Status;
    }

    m_pProcess = pProcess.Detach();
    m_pDebug = pCorDebug.Detach();

    m_processId = pid;

    return S_OK;
}

HRESULT Debugger::RunProcess(std::string fileExec, std::vector<std::string> execArgs)
{
    static const auto startupCallbackWaitTimeout = std::chrono::milliseconds(5000);
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    std::stringstream ss;
    ss << "\"" << fileExec << "\"";
    for (std::string &arg : execArgs)
    {
        ss << " \"" << MIProtocol::EscapeMIValue(arg) << "\"";
    }
    std::string cmdString = ss.str();
    std::unique_ptr<WCHAR[]> cmd(new WCHAR[cmdString.size() + 1]);

    MultiByteToWideChar(CP_UTF8, 0, cmdString.c_str(), cmdString.size() + 1, &cmd[0], cmdString.size() + 1);

    m_startupReady = false;
    m_clrPath.clear();

    BOOL bSuspendProcess = TRUE;
    LPVOID lpEnvironment = nullptr; // as current
    LPCWSTR lpCurrentDirectory = nullptr; // as current
    HANDLE resumeHandle;
    IfFailRet(CreateProcessForLaunch(&cmd[0], bSuspendProcess, lpEnvironment, lpCurrentDirectory, &m_processId, &resumeHandle));

    IfFailRet(RegisterForRuntimeStartup(m_processId, Debugger::StartupCallback, this, &m_unregisterToken));

    // Resume the process so that StartupCallback can run
    IfFailRet(ResumeProcess(resumeHandle));
    CloseResumeHandle(resumeHandle);

    // Wait for Debugger::StartupCallback to complete

    // FIXME: if the process exits too soon the Debugger::StartupCallback()
    // is never called (bug in dbgshim?).
    // The workaround is to wait with timeout.
    auto now = std::chrono::system_clock::now();

    std::unique_lock<std::mutex> lock(m_startupMutex);
    if (!m_startupCV.wait_until(lock, now + startupCallbackWaitTimeout, [this](){return m_startupReady;}))
    {
        // Timed out
        UnregisterForRuntimeStartup(m_unregisterToken);
        m_unregisterToken = nullptr;
        return E_FAIL;
    }

    return m_startupResult;
}

HRESULT Debugger::CheckNoProcess()
{
    if (m_pProcess || m_pDebug)
    {
        std::lock_guard<std::mutex> lock(m_processAttachedMutex);
        if (m_processAttachedState == ProcessAttached)
            return E_FAIL; // Already attached
        m_processAttachedMutex.unlock();

        TerminateProcess();
    }
    return S_OK;
}

HRESULT Debugger::DetachFromProcess()
{
    if (!m_pProcess || !m_pDebug)
        return E_FAIL;

    if (SUCCEEDED(m_pProcess->Stop(0)))
    {
        DeleteAllBreakpoints();
        DisableAllBreakpointsAndSteppers(m_pProcess);
        m_pProcess->Detach();
    }

    Cleanup();

    m_pProcess->Release();
    m_pProcess = nullptr;

    m_pDebug->Terminate();
    m_pDebug = nullptr;

    return S_OK;
}

HRESULT Debugger::TerminateProcess()
{
    if (!m_pProcess || !m_pDebug)
        return E_FAIL;

    if (SUCCEEDED(m_pProcess->Stop(0)))
    {
        DisableAllBreakpointsAndSteppers(m_pProcess);
        //pProcess->Detach();
    }

    Cleanup();

    m_pProcess->Terminate(0);
    WaitProcessExited();

    m_pProcess->Release();
    m_pProcess = nullptr;

    m_pDebug->Terminate();
    m_pDebug = nullptr;

    return S_OK;
}

void Debugger::Cleanup()
{
    m_modules.CleanupAllModules();
    m_evaluator.Cleanup();
    m_protocol->Cleanup();
    // TODO: Cleanup libcoreclr.so instance
}

HRESULT Debugger::AttachToProcess(int pid)
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    std::string m_clrPath = GetCLRPath(pid);
    if (m_clrPath.empty())
        return E_INVALIDARG; // Unable to find libcoreclr.so

    WCHAR szModuleName[MAX_LONGPATH];
    MultiByteToWideChar(CP_UTF8, 0, m_clrPath.c_str(), m_clrPath.size() + 1, szModuleName, MAX_LONGPATH);

    WCHAR pBuffer[100];
    DWORD dwLength;
    IfFailRet(CreateVersionStringFromModule(
        pid,
        szModuleName,
        pBuffer,
        _countof(pBuffer),
        &dwLength));

    ToRelease<IUnknown> pCordb;

    IfFailRet(CreateDebuggingInterfaceFromVersionEx(4, pBuffer, &pCordb));

    m_unregisterToken = nullptr;
    return Startup(pCordb, pid);
}

void print_help()
{
    fprintf(stderr,
        ".NET Core debugger for Linux/macOS.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n");
}

int main(int argc, char *argv[])
{
    DWORD pidDebuggee = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--attach") == 0)
        {
            i++;
            if (i >= argc)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
            char *err;
            pidDebuggee = strtoul(argv[i], &err, 10);
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "--interpreter=mi") == 0)
        {
            continue;
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            print_help();
            return EXIT_SUCCESS;
        }
        else
        {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    Debugger debugger;

    MIProtocol protocol;
    protocol.SetDebugger(&debugger);

    if (pidDebuggee != 0)
    {
        HRESULT Status = debugger.AttachToProcess(pidDebuggee);
        if (FAILED(Status))
        {
            fprintf(stderr, "Error: 0x%x Failed to attach to %i\n", Status, pidDebuggee);
            return EXIT_FAILURE;
        }
    }

    protocol.CommandLoop();

    return EXIT_SUCCESS;
}
