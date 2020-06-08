// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "manageddebugger.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <stdexcept>

#include "valueprint.h"
#include "symbolreader.h"
#include "cputil.h"
#include "platform.h"
#include "typeprinter.h"
#include "frames.h"
#include "logger.h"

using std::string;

// From dbgshim.h
struct dbgshim_t
{
    typedef VOID (*PSTARTUP_CALLBACK)(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT (*CreateProcessForLaunch)(LPWSTR lpCommandLine, BOOL bSuspendProcess, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, PDWORD pProcessId, HANDLE *pResumeHandle);
    HRESULT (*ResumeProcess)(HANDLE hResumeHandle);
    HRESULT (*CloseResumeHandle)(HANDLE hResumeHandle);
    HRESULT (*RegisterForRuntimeStartup)(DWORD dwProcessId, PSTARTUP_CALLBACK pfnCallback, PVOID parameter, PVOID *ppUnregisterToken);
    HRESULT (*UnregisterForRuntimeStartup)(PVOID pUnregisterToken);
    HRESULT (*EnumerateCLRs)(DWORD debuggeePID, HANDLE** ppHandleArrayOut, LPWSTR** ppStringArrayOut, DWORD* pdwArrayLengthOut);
    HRESULT (*CloseCLREnumeration)(HANDLE* pHandleArray, LPWSTR* pStringArray, DWORD dwArrayLength);
    HRESULT (*CreateVersionStringFromModule)(DWORD pidDebuggee, LPCWSTR szModuleName, LPWSTR pBuffer, DWORD cchBuffer, DWORD* pdwLength);
    HRESULT (*CreateDebuggingInterfaceFromVersionEx)(int iDebuggerVersion, LPCWSTR szDebuggeeVersion, IUnknown ** ppCordb);

    dbgshim_t() :
        CreateProcessForLaunch(nullptr),
        ResumeProcess(nullptr),
        CloseResumeHandle(nullptr),
        RegisterForRuntimeStartup(nullptr),
        UnregisterForRuntimeStartup(nullptr),
        EnumerateCLRs(nullptr),
        CloseCLREnumeration(nullptr),
        CreateVersionStringFromModule(nullptr),
        CreateDebuggingInterfaceFromVersionEx(nullptr),
        m_module(nullptr)
    {
#ifdef DBGSHIM_RUNTIME_DIR
        string libName(DBGSHIM_RUNTIME_DIR);
        libName += DIRECTORY_SEPARATOR_STR_A;
#else
        string exe = GetExeAbsPath();
        std::size_t dirSepIndex = exe.rfind(DIRECTORY_SEPARATOR_STR_A);
        if (dirSepIndex == string::npos)
            return;
        string libName = exe.substr(0, dirSepIndex + 1);
#endif

#ifdef WIN32
        libName += "dbgshim.dll";
#elif defined(__APPLE__)
        libName += "libdbgshim.dylib";
#else
        libName += "libdbgshim.so";
#endif

        m_module = DLOpen(libName);
        if (!m_module)
            throw std::invalid_argument("Unable to load " + libName);

        *((void**)&CreateProcessForLaunch) = DLSym(m_module, "CreateProcessForLaunch");
        *((void**)&ResumeProcess) = DLSym(m_module, "ResumeProcess");
        *((void**)&CloseResumeHandle) = DLSym(m_module, "CloseResumeHandle");
        *((void**)&RegisterForRuntimeStartup) = DLSym(m_module, "RegisterForRuntimeStartup");
        *((void**)&UnregisterForRuntimeStartup) = DLSym(m_module, "UnregisterForRuntimeStartup");
        *((void**)&EnumerateCLRs) = DLSym(m_module, "EnumerateCLRs");
        *((void**)&CloseCLREnumeration) = DLSym(m_module, "CloseCLREnumeration");
        *((void**)&CreateVersionStringFromModule) = DLSym(m_module, "CreateVersionStringFromModule");
        *((void**)&CreateDebuggingInterfaceFromVersionEx) = DLSym(m_module, "CreateDebuggingInterfaceFromVersionEx");
    }
    ~dbgshim_t()
    {
        // if (m_module)
        //     DLClose(m_module);
    }
private:
    void *m_module;
};

static dbgshim_t g_dbgshim;

void ManagedDebugger::NotifyProcessCreated()
{
    std::lock_guard<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessAttached;
}

void ManagedDebugger::NotifyProcessExited()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessUnattached;
    lock.unlock();
    m_processAttachedCV.notify_one();
}

void ManagedDebugger::WaitProcessExited()
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

HRESULT ManagedDebugger::DisableAllSteppers(ICorDebugProcess *pProcess)
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

void ManagedDebugger::SetLastStoppedThread(ICorDebugThread *pThread)
{
    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    m_lastStoppedThreadId = threadId;
}

int ManagedDebugger::GetLastStoppedThreadId()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    return m_lastStoppedThreadId;
}

static HRESULT GetExceptionInfo(ICorDebugThread *pThread,
                                string &excType,
                                string &excModule)
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

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2, ICorDebugManagedCallback3
{
    ULONG m_refCount;
    ManagedDebugger &m_debugger;
public:

        void HandleEvent(ICorDebugController *controller, const string &eventName)
        {
            LogFuncEntry();

            string text = "Event received: '" + eventName + "'\n";
            m_debugger.m_protocol->EmitOutputEvent(OutputEvent(OutputConsole, text));
            controller->Continue(0);
        }

        ManagedCallback(ManagedDebugger &debugger) : m_refCount(1), m_debugger(debugger) {}
        virtual ~ManagedCallback() {}

        // IUnknown

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface)
        {
            LogFuncEntry();

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
            else if(riid == __uuidof(ICorDebugManagedCallback3))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback3*>(this);
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
            LogFuncEntry();

            return InterlockedIncrement((volatile LONG *) &m_refCount);
        }

        virtual ULONG STDMETHODCALLTYPE Release()
        {
            LogFuncEntry();

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
            LogFuncEntry();

            if (m_debugger.m_evaluator.IsEvalRunning())
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
                DWORD threadId = 0;
                pThread->GetID(&threadId);

                bool atEntry = false;
                StoppedEvent event(StopBreakpoint, threadId);
                if (FAILED(m_debugger.m_breakpoints.HitBreakpoint(&m_debugger, pThread, pBreakpoint, event.breakpoint, atEntry)))
                {
                    pAppDomain->Continue(0);
                    return;
                }

                if (atEntry)
                    event.reason = StopEntry;

                ToRelease<ICorDebugFrame> pFrame;
                if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                    m_debugger.GetFrameLocation(pFrame, threadId, 0, event.frame);

                m_debugger.SetLastStoppedThread(pThread);
                m_debugger.m_protocol->EmitStoppedEvent(event);

            },
                std::move(callbackAppDomain),
                std::move(callbackThread),
                std::move(callbackBreakpoint)
            ).detach();

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE StepComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugStepper *pStepper,
            /* [in] */ CorDebugStepReason reason)
        {
            LogFuncEntry();

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            StackFrame stackFrame;
            ToRelease<ICorDebugFrame> pFrame;
            HRESULT Status = S_FALSE;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                Status = m_debugger.GetFrameLocation(pFrame, threadId, 0, stackFrame);

            const bool no_source = Status == S_FALSE;

            if (m_debugger.IsJustMyCode() && no_source)
            {
                m_debugger.SetupStep(pThread, Debugger::STEP_OVER);
                pAppDomain->Continue(0);
            }
            else
            {
                StoppedEvent event(StopStep, threadId);
                event.frame = stackFrame;

                m_debugger.SetLastStoppedThread(pThread);
                m_debugger.m_protocol->EmitStoppedEvent(event);
            }
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ BOOL unhandled)
        {
            // Obsolete callback
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            LogFuncEntry();

            HRESULT Status = S_OK;

            m_debugger.m_evaluator.NotifyEvalComplete(pThread, pEval);

            if (m_debugger.m_evaluator.is_empty_eval_queue())
            {
                pAppDomain->SetAllThreadsDebugState(THREAD_RUN, nullptr);
            }
            else
            {
                DWORD currentThreadId;
                pThread->GetID(&currentThreadId);
                DWORD evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                if (evalThreadId == currentThreadId) {
                    m_debugger.m_evaluator.pop_eval_queue();

                    evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                    ToRelease<ICorDebugThread> pThreadEval;
                    IfFailRet(m_debugger.m_pProcess->GetThread(evalThreadId, &pThreadEval));
                    IfFailRet(pAppDomain->SetAllThreadsDebugState(THREAD_SUSPEND, nullptr));
                    IfFailRet(pThreadEval->SetDebugState(THREAD_RUN));

                    Logger::levelLog(LOG_INFO, "Complete eval threadid = '%d'", currentThreadId);
                }
                else {
                    Logger::levelLog(LOG_ERROR, "Logical error: eval queue '%d' != '%d'", currentThreadId, evalThreadId);
                }
            }
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalException(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            LogFuncEntry();

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
                DWORD currentThreadId;
                pThread->GetID(&currentThreadId);
                DWORD evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                if (evalThreadId == currentThreadId) {
                    m_debugger.m_evaluator.pop_eval_queue();

                    evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                    ToRelease<ICorDebugThread> pThreadEval;
                    IfFailRet(m_debugger.m_pProcess->GetThread(evalThreadId, &pThreadEval));
                    IfFailRet(pAppDomain->SetAllThreadsDebugState(THREAD_SUSPEND, nullptr));
                    IfFailRet(pThreadEval->SetDebugState(THREAD_RUN));

                    Logger::levelLog(LOG_INFO, "Eval exception, threadid = '%d'", currentThreadId);
                }
                else {
                    Logger::levelLog(LOG_ERROR, "Logical error: eval queue '%d' != '%d'", currentThreadId, evalThreadId);
                }
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            LogFuncEntry();
            m_debugger.NotifyProcessCreated();
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            LogFuncEntry();

            if (m_debugger.m_evaluator.IsEvalRunning())
            {
                Logger::levelLog(LOG_WARN, "The target process exited while evaluating the function.");
            }

            m_debugger.m_evaluator.NotifyEvalComplete(nullptr, nullptr);

            while (!m_debugger.m_evaluator.is_empty_eval_queue())
                m_debugger.m_evaluator.pop_eval_queue();

            m_debugger.m_protocol->EmitExitedEvent(ExitedEvent(0));
            m_debugger.NotifyProcessExited();
            m_debugger.m_protocol->EmitTerminatedEvent();

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            LogFuncEntry();

            DWORD threadId = 0;
            thread->GetID(&threadId);
            m_debugger.m_protocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            LogFuncEntry();

            // TODO: clean evaluations and exceptions queues for current thread

            m_debugger.m_evaluator.NotifyEvalComplete(thread, nullptr);
            DWORD threadId = 0;
            thread->GetID(&threadId);
            m_debugger.m_protocol->EmitThreadEvent(ThreadEvent(ThreadExited, threadId));
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadModule(
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

        virtual HRESULT STDMETHODCALLTYPE UnloadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE DebuggerError(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ HRESULT errorHR,
            /* [in] */ DWORD errorCode)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE LogMessage(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pMessage)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE LogSwitch(
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

        virtual HRESULT STDMETHODCALLTYPE CreateAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE ControlCTrap(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE NameChange(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule,
            /* [in] */ IStream *pSymbolStream)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction,
            /* [in] */ BOOL fAccurate)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE BreakpointSetError(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint,
            /* [in] */ DWORD dwError)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }


        // ICorDebugManagedCallback2

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pOldFunction,
            /* [in] */ ICorDebugFunction *pNewFunction,
            /* [in] */ ULONG32 oldILOffset)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId,
            /* [in] */ WCHAR *pConnName)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE ChangeConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE DestroyConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE Exception(
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
            DWORD threadId = 0;
            IfFailRet(pThread->GetID(&threadId));
            string excType, excModule;
            IfFailRet(GetExceptionInfo(pThread, excType, excModule));

            ExceptionBreakMode mode;
            m_debugger.m_breakpoints.GetExceptionBreakMode(mode, "*");
            bool unhandled = (dwEventType == DEBUG_EXCEPTION_UNHANDLED && mode.Unhandled());
            bool not_matched = !(unhandled || m_debugger.MatchExceptionBreakpoint(dwEventType, excType, ExceptionBreakCategory::CLR));

            if (not_matched) {
                string text = "Exception thrown: '" + excType + "' in " + excModule + "\n";
                OutputEvent event(OutputConsole, text);
                event.source = "target-exception";
                m_debugger.m_protocol->EmitOutputEvent(event);
                IfFailRet(pAppDomain->Continue(0));
                return S_OK;
            }

            StoppedEvent event(StopException, threadId);

            string details;
            if (unhandled) {
                details = "An unhandled exception of type '" + excType + "' occurred in " + excModule;
                std::lock_guard<std::mutex> lock(m_debugger.m_lastUnhandledExceptionThreadIdsMutex);
                m_debugger.m_lastUnhandledExceptionThreadIds.insert(threadId);
            }
            else {
                details = "Exception thrown: '" + excType + "' in " + excModule;
            }

            string message;
            WCHAR fieldName[] = W("_message\0");
            ToRelease<ICorDebugValue> pExceptionValue;
            IfFailRet(pThread->GetCurrentException(&pExceptionValue));
            IfFailRet(PrintStringField(pExceptionValue, fieldName, message));

            StackFrame stackFrame;
            ToRelease<ICorDebugFrame> pActiveFrame;
            if (SUCCEEDED(pThread->GetActiveFrame(&pActiveFrame)) && pActiveFrame != nullptr)
                m_debugger.GetFrameLocation(pActiveFrame, threadId, 0, stackFrame);

            m_debugger.SetLastStoppedThread(pThread);

            event.text = excType;
            event.description = message.empty() ? details : message;
            event.frame = stackFrame;

            if (m_debugger.m_evaluator.IsEvalRunning() && !m_debugger.m_evaluator.is_empty_eval_queue()) {
                DWORD evalThreadId = m_debugger.m_evaluator.front_eval_queue();
                ToRelease<ICorDebugThread> pThreadEval;
                IfFailRet(m_debugger.m_pProcess->GetThread(evalThreadId, &pThreadEval));
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

            m_debugger.Stop(threadId, event);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
            /* [in] */ DWORD dwFlags)
        {
            HRESULT Status;
            DWORD threadId = 0;
            IfFailRet(pThread->GetID(&threadId));
            // We produce DEBUG_EXCEPTION_INTERCEPTED from Exception() callback.
            // TODO: we should waiting this unwinding on exit().
            Logger::levelLog(LOG_INFO, "ExceptionUnwind:threadId:%d,dwEventType:%d,dwFlags:%d", threadId, dwEventType, dwFlags);
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction)
        {
            LogFuncEntry();
            return E_NOTIMPL;
        }

        virtual HRESULT STDMETHODCALLTYPE MDANotification(
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

        // ICorDebugManagedCallback3

        virtual HRESULT STDMETHODCALLTYPE CustomNotification(
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
                if (threadEval != nullptr) {
                    IfFailRet(threadEval->Abort());
                }
            }

            IfFailRet(pAppDomain->Continue(false));
            return S_OK;
        }
};

ManagedDebugger::ManagedDebugger() :
    m_processAttachedState(ProcessUnattached),
    m_lastStoppedThreadId(-1),
    m_stopCounter(0),
    m_startMethod(StartNone),
    m_stopAtEntry(false),
    m_isConfigurationDone(false),
    m_evaluator(m_modules),
    m_breakpoints(m_modules),
    m_variables(m_evaluator),
    m_protocol(nullptr),
    m_managedCallback(new ManagedCallback(*this)),
    m_pDebug(nullptr),
    m_pProcess(nullptr),
    m_justMyCode(true),
    m_startupReady(false),
    m_startupResult(S_OK),
    m_unregisterToken(nullptr),
    m_processId(0)
{
}

ManagedDebugger::~ManagedDebugger()
{
}

HRESULT ManagedDebugger::Initialize()
{
    LogFuncEntry();

    // TODO: Report capabilities and check client support
    m_startMethod = StartNone;
    m_protocol->EmitInitializedEvent();
    return S_OK;
}

HRESULT ManagedDebugger::RunIfReady()
{
    if (m_startMethod == StartNone || !m_isConfigurationDone)
        return S_OK;

    switch(m_startMethod)
    {
        case StartLaunch:
            return RunProcess(m_execPath, m_execArgs);
        case StartAttach:
            return AttachToProcess(m_processId);
        default:
            return E_FAIL;
    }

    //Unreachable
    return E_FAIL;
}

HRESULT ManagedDebugger::Attach(int pid)
{
    LogFuncEntry();

    m_startMethod = StartAttach;
    m_processId = pid;
    return RunIfReady();
}

HRESULT ManagedDebugger::Launch(string fileExec, std::vector<string> execArgs, bool stopAtEntry)
{
    LogFuncEntry();

    m_startMethod = StartLaunch;
    m_execPath = fileExec;
    m_execArgs = execArgs;
    m_stopAtEntry = stopAtEntry;
    m_breakpoints.SetStopAtEntry(m_stopAtEntry);
    return RunIfReady();
}

HRESULT ManagedDebugger::ConfigurationDone()
{
    LogFuncEntry();

    m_isConfigurationDone = true;

    return RunIfReady();
}

HRESULT ManagedDebugger::Disconnect(DisconnectAction action)
{
    LogFuncEntry();

    bool terminate;
    switch(action)
    {
        case DisconnectDefault:
            switch(m_startMethod)
            {
                case StartLaunch:
                    terminate = true;
                    break;
                case StartAttach:
                    terminate = false;
                    break;
                default:
                    return E_FAIL;
            }
            break;
        case DisconnectTerminate:
            terminate = true;
            break;
        case DisconnectDetach:
            terminate = false;
            break;
        default:
            return E_FAIL;
    }

    if (!terminate)
    {
        HRESULT Status = DetachFromProcess();
        if (SUCCEEDED(Status))
            m_protocol->EmitTerminatedEvent();
        return Status;
    }

    return TerminateProcess();
}

HRESULT ManagedDebugger::SetupStep(ICorDebugThread *pThread, Debugger::StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    CorDebugUnmappedStop stopMask = STOP_NONE;
    IfFailRet(pStepper->SetUnmappedStopMask(stopMask));

    ToRelease<ICorDebugStepper2> pStepper2;
    IfFailRet(pStepper->QueryInterface(IID_ICorDebugStepper2, (LPVOID *)&pStepper2));

    IfFailRet(pStepper2->SetJMC(IsJustMyCode()));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());
        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(m_modules.GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    return S_OK;
}

HRESULT ManagedDebugger::StepCommand(int threadId, StepType stepType)
{
    LogFuncEntry();

    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(threadId, &pThread));
    DisableAllSteppers(m_pProcess);
    IfFailRet(SetupStep(pThread, stepType));

    m_variables.Clear();
    Status = m_pProcess->Continue(0);

    if (SUCCEEDED(Status)) {
        m_protocol->EmitContinuedEvent(threadId);
        --m_stopCounter;
    }
    return Status;
}

HRESULT ManagedDebugger::Stop(int threadId, const StoppedEvent &event)
{
    LogFuncEntry();
    HRESULT Status = S_OK;

    while (m_stopCounter.load() > 0) {
        m_protocol->EmitContinuedEvent(m_lastStoppedThreadId);
        --m_stopCounter;
    }
    // INFO: Double EmitStopEvent() produce blocked coreclr command reader
    m_stopCounter.store(1); // store zero and increment
    m_protocol->EmitStoppedEvent(event);

    return Status;
}

HRESULT ManagedDebugger::Continue(int threadId)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;

    HRESULT res = S_OK;
    if (!m_evaluator.IsEvalRunning() && m_evaluator.is_empty_eval_queue()) {
        if ((res = m_pProcess->SetAllThreadsDebugState(THREAD_RUN, nullptr)) != S_OK) {
            // TODO: need function for printing coreCLR errors by error code
            switch (res) {
                case CORDBG_E_PROCESS_NOT_SYNCHRONIZED:
                    Logger::levelLog(LOG_ERROR, "Setting thread state failed. Process not synchronized:'%0x'", res);
                break;
                case CORDBG_E_PROCESS_TERMINATED:
                    Logger::levelLog(LOG_ERROR, "Setting thread state failed. Process was terminated:'%0x'", res);
                break;
                case CORDBG_E_OBJECT_NEUTERED:
                    Logger::levelLog(LOG_ERROR, "Setting thread state failed. Object has been neutered(it's in a zombie state):'%0x'", res);
                break;
                default:
                    Logger::levelLog(LOG_ERROR, "SetAllThreadsDebugState() %0x", res);
                break;
            }
        }
    }
    if ((res = m_pProcess->Continue(0)) != S_OK) {
        switch (res) {
        case CORDBG_E_SUPERFLOUS_CONTINUE:
            Logger::levelLog(LOG_ERROR, "Continue failed. Returned from a call to Continue that was not matched with a stopping event:'%0x'", res);
            break;
        case CORDBG_E_PROCESS_TERMINATED:
            Logger::levelLog(LOG_ERROR, "Continue failed. Process was terminated:'%0x'", res);
            break;
        case CORDBG_E_OBJECT_NEUTERED:
            Logger::levelLog(LOG_ERROR, "Continue failed. Object has been neutered(it's in a zombie state):'%0x'", res);
            break;
        default:
            Logger::levelLog(LOG_ERROR, "Continue() %0x", res);
            break;
        }
    }

    if (SUCCEEDED(res)) {
        m_protocol->EmitContinuedEvent(threadId);
        --m_stopCounter;
    }

    return res;
}

HRESULT ManagedDebugger::Pause()
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;

    // The debugger maintains a stop counter. When the counter goes to zero, the controller is resumed.
    // Each call to Stop or each dispatched callback increments the counter.
    // Each call to ICorDebugController::Continue decrements the counter.
    BOOL running = FALSE;
    HRESULT Status = m_pProcess->IsRunning(&running);
    if (Status != S_OK)
        return Status;
    if (!running)
        return S_OK;

    Status = m_pProcess->Stop(0);
    if (Status != S_OK)
        return Status;

    // For Visual Studio, we have to report a thread ID in async stop event.
    // We have to find a thread which has a stack frame with valid location in its stack trace.
    std::vector<Thread> threads;
    GetThreads(threads);

    int lastStoppedId = GetLastStoppedThreadId();

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

        if (FAILED(GetStackTrace(thread.id, 0, 0, stackFrames, totalFrames)))
            continue;

        for (const StackFrame& stackFrame : stackFrames)
        {
            if (stackFrame.source.IsNull())
                continue;

            StoppedEvent event(StopPause, thread.id);
            event.frame = stackFrame;
            m_protocol->EmitStoppedEvent(event);

            return Status;
        }
    }

    m_protocol->EmitStoppedEvent(StoppedEvent(StopPause, 0));

    return Status;
}

HRESULT ManagedDebugger::GetThreads(std::vector<Thread> &threads)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;
    return GetThreadsState(m_pProcess, threads);
}

HRESULT ManagedDebugger::GetStackTrace(int threadId, int startFrame, int levels, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    HRESULT Status;
    if (!m_pProcess)
        return E_FAIL;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(threadId, &pThread));
    return GetStackTrace(pThread, startFrame, levels, stackFrames, totalFrames);
}

VOID ManagedDebugger::StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr)
{
    ManagedDebugger *self = static_cast<ManagedDebugger*>(parameter);

    std::unique_lock<std::mutex> lock(self->m_startupMutex);

    self->m_startupResult = FAILED(hr) ? hr : self->Startup(pCordb, self->m_processId);
    self->m_startupReady = true;

    if (self->m_unregisterToken)
    {
        g_dbgshim.UnregisterForRuntimeStartup(self->m_unregisterToken);
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

static HRESULT InternalEnumerateCLRs(
    DWORD pid, HANDLE **ppHandleArray, LPWSTR **ppStringArray, DWORD *pdwArrayLength, int tryCount)
{
    int numTries = 0;
    HRESULT hr;

    while (numTries < tryCount)
    {
        hr = g_dbgshim.EnumerateCLRs(pid, ppHandleArray, ppStringArray, pdwArrayLength);

        // From dbgshim.cpp:
        // EnumerateCLRs uses the OS API CreateToolhelp32Snapshot which can return ERROR_BAD_LENGTH or
        // ERROR_PARTIAL_COPY. If we get either of those, we try wait 1/10th of a second try again (that
        // is the recommendation of the OS API owners).
        // In dbgshim the following condition is used:
        //  if ((hr != HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY)) && (hr != HRESULT_FROM_WIN32(ERROR_BAD_LENGTH)))
        // Since we may be attaching to the process which has not loaded coreclr yes, let's give it some time to load.
        if (SUCCEEDED(hr))
        {
            // Just return any other error or if no handles were found (which means the coreclr module wasn't found yet).
            if (*ppHandleArray != NULL && *pdwArrayLength > 0)
            {

                // If EnumerateCLRs succeeded but any of the handles are INVALID_HANDLE_VALUE, then sleep and retry
                // also. This fixes a race condition where dbgshim catches the coreclr module just being loaded but
                // before g_hContinueStartupEvent has been initialized.
                if (AreAllHandlesValid(*ppHandleArray, *pdwArrayLength))
                {
                    return hr;
                }
                // Clean up memory allocated in EnumerateCLRs since this path it succeeded
                g_dbgshim.CloseCLREnumeration(*ppHandleArray, *ppStringArray, *pdwArrayLength);

                *ppHandleArray = NULL;
                *ppStringArray = NULL;
                *pdwArrayLength = 0;
            }
        }

        // No point in retrying in case of invalid arguments or no such process
        if (hr == E_INVALIDARG || hr == E_FAIL)
            return hr;

        // Sleep and retry enumerating the runtimes
        USleep(100*1000);
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

static string GetCLRPath(DWORD pid, int timeoutSec = 3)
{
    HANDLE* pHandleArray;
    LPWSTR* pStringArray;
    DWORD dwArrayLength;
    const int tryCount = timeoutSec * 10; // 100ms interval between attempts
    if (FAILED(InternalEnumerateCLRs(pid, &pHandleArray, &pStringArray, &dwArrayLength, tryCount)) || dwArrayLength == 0)
        return string();

    string result = to_utf8(pStringArray[0]);

    g_dbgshim.CloseCLREnumeration(pHandleArray, pStringArray, dwArrayLength);

    return result;
}

HRESULT ManagedDebugger::Startup(IUnknown *punk, DWORD pid)
{
    LogFuncEntry();

    HRESULT Status;

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

static string EscapeShellArg(const string &arg)
{
    string s(arg);

    for (string::size_type i = 0; i < s.size(); ++i)
    {
        string::size_type count = 0;
        char c = s.at(i);
        switch (c)
        {
            case '\"': count = 1; s.insert(i, count, '\\'); s[i + count] = '\"'; break;
            case '\\': count = 1; s.insert(i, count, '\\'); s[i + count] = '\\'; break;
        }
        i += count;
    }

    return s;
}

HRESULT ManagedDebugger::RunProcess(string fileExec, std::vector<string> execArgs)
{
    static const auto startupCallbackWaitTimeout = std::chrono::milliseconds(5000);
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    std::ostringstream ss;
    ss << "\"" << fileExec << "\"";
    for (const string &arg : execArgs)
        ss << " \"" << EscapeShellArg(arg) << "\"";

    m_startupReady = false;
    m_clrPath.clear();

    HANDLE resumeHandle; // Fake thread handle for the process resume

    IfFailRet(g_dbgshim.CreateProcessForLaunch(reinterpret_cast<LPWSTR>(const_cast<WCHAR*>(to_utf16(ss.str()).c_str())),
                                     /* Suspend process */ TRUE,
                                     /* Current environment */ NULL,
                                     /* Current working directory */ NULL,
                                     &m_processId, &resumeHandle));

    IfFailRet(g_dbgshim.RegisterForRuntimeStartup(m_processId, ManagedDebugger::StartupCallback, this, &m_unregisterToken));

    // Resume the process so that StartupCallback can run
    IfFailRet(g_dbgshim.ResumeProcess(resumeHandle));
    g_dbgshim.CloseResumeHandle(resumeHandle);

    // Wait for ManagedDebugger::StartupCallback to complete

    /// FIXME: if the process exits too soon the ManagedDebugger::StartupCallback()
    /// is never called (bug in dbgshim?).
    /// The workaround is to wait with timeout.
    const auto now = std::chrono::system_clock::now();

    std::unique_lock<std::mutex> lock(m_startupMutex);
    if (!m_startupCV.wait_until(lock, now + startupCallbackWaitTimeout, [this](){return m_startupReady;}))
    {
        // Timed out
        g_dbgshim.UnregisterForRuntimeStartup(m_unregisterToken);
        m_unregisterToken = nullptr;
        return E_FAIL;
    }

    return m_startupResult;
}

HRESULT ManagedDebugger::CheckNoProcess()
{
    if (m_pProcess || m_pDebug)
    {
        std::unique_lock<std::mutex> lock(m_processAttachedMutex);
        if (m_processAttachedState == ProcessAttached)
            return E_FAIL; // Already attached
        lock.unlock();

        TerminateProcess();
    }
    return S_OK;
}

HRESULT ManagedDebugger::DetachFromProcess()
{
    if (!m_pProcess || !m_pDebug)
        return E_FAIL;

    if (SUCCEEDED(m_pProcess->Stop(0)))
    {
        m_breakpoints.DeleteAllBreakpoints();
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

HRESULT ManagedDebugger::TerminateProcess()
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

void ManagedDebugger::Cleanup()
{
    m_modules.CleanupAllModules();
    m_evaluator.Cleanup();
    m_protocol->Cleanup();
    // TODO: Cleanup libcoreclr.so instance
}

HRESULT ManagedDebugger::AttachToProcess(DWORD pid)
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    m_clrPath = GetCLRPath(pid);
    if (m_clrPath.empty())
        return E_INVALIDARG; // Unable to find libcoreclr.so

    WCHAR pBuffer[100];
    DWORD dwLength;
    IfFailRet(g_dbgshim.CreateVersionStringFromModule(
        pid,
        reinterpret_cast<LPCWSTR>(to_utf16(m_clrPath).c_str()),
        pBuffer,
        _countof(pBuffer),
        &dwLength));

    ToRelease<IUnknown> pCordb;

    IfFailRet(g_dbgshim.CreateDebuggingInterfaceFromVersionEx(CorDebugVersion_4_0, pBuffer, &pCordb));

    m_unregisterToken = nullptr;
    return Startup(pCordb, pid);
}

// VSCode
HRESULT ManagedDebugger::GetExceptionInfoResponse(int threadId,
    ExceptionInfoResponse &exceptionInfoResponse)
{
    LogFuncEntry();

    // Are needed to move next line to Exception() callback?
    m_evaluator.push_eval_queue(threadId);

    HRESULT res = E_FAIL;
    HRESULT Status = S_OK;
    bool hasInner = false;
    Variable varException;
    ToRelease <ICorDebugValue> evalValue;
    ToRelease<ICorDebugValue> pExceptionValue;
    ToRelease<ICorDebugThread> pThread;

    WCHAR message[] = W("_message\0");
    uint64_t frameId = StackFrame(threadId, 0, "").id;

    std::unique_lock<std::mutex> lock(m_lastUnhandledExceptionThreadIdsMutex);
    if (m_lastUnhandledExceptionThreadIds.find(threadId) != m_lastUnhandledExceptionThreadIds.end()) {
        lock.unlock();
        exceptionInfoResponse.breakMode.resetAll();
    }
    else {
        lock.unlock();
        ExceptionBreakMode mode;

        if ((res = m_breakpoints.GetExceptionBreakMode(mode, "*")) && FAILED(res))
            goto failed;

        exceptionInfoResponse.breakMode = mode;
    }

    if ((res = m_pProcess->GetThread(threadId, &pThread)) && FAILED(res))
        goto failed;

    if ((res = pThread->GetCurrentException(&pExceptionValue)) && FAILED(res)) {
        Logger::levelLog(LOG_ERROR, "GetCurrentException() failed, %0x", res);
        goto failed;
    }

    PrintStringField(pExceptionValue, message, exceptionInfoResponse.description);

    if ((res = m_variables.GetExceptionVariable(frameId, pThread, varException)) && FAILED(res))
        goto failed;

    if (exceptionInfoResponse.breakMode.OnlyUnhandled() || exceptionInfoResponse.breakMode.UserUnhandled()) {
        exceptionInfoResponse.description = "An unhandled exception of type '" + varException.type +
            "' occurred in " + varException.module;
    }
    else {
        exceptionInfoResponse.description = "Exception thrown: '" + varException.type +
            "' in " + varException.module;
    }

    exceptionInfoResponse.exceptionId = varException.type;

    exceptionInfoResponse.details.evaluateName = varException.name;
    exceptionInfoResponse.details.typeName = varException.type;
    exceptionInfoResponse.details.fullTypeName = varException.type;

    if (FAILED(m_evaluator.getObjectByFunction("get_StackTrace", pThread, pExceptionValue, &evalValue, defaultEvalFlags))) {
        exceptionInfoResponse.details.stackTrace = "<undefined>"; // Evaluating problem entire object
    }
    else {
        PrintValue(evalValue, exceptionInfoResponse.details.stackTrace);
        ToRelease <ICorDebugValue> evalValueOut;
        BOOL isNotNull = TRUE;

        ICorDebugValue *evalValueInner = pExceptionValue;
        while (isNotNull) {
            if ((res = m_evaluator.getObjectByFunction("get_InnerException", pThread, evalValueInner, &evalValueOut, defaultEvalFlags)) && FAILED(res))
                goto failed;

            string tmpstr;
            PrintValue(evalValueOut, tmpstr);

            if (tmpstr.compare("null") == 0)
                break;

            ToRelease<ICorDebugValue> pValueTmp;

            if ((res = DereferenceAndUnboxValue(evalValueOut, &pValueTmp, &isNotNull)) && FAILED(res))
                goto failed;

            hasInner = true;
            ExceptionDetails inner;
            PrintStringField(evalValueOut, message, inner.message);

            if ((res = m_evaluator.getObjectByFunction("get_StackTrace", pThread, evalValueOut, &pValueTmp, defaultEvalFlags)) && FAILED(res))
                goto failed;

            PrintValue(pValueTmp, inner.stackTrace);

            exceptionInfoResponse.details.innerException.push_back(inner);
            evalValueInner = evalValueOut;
        }
    }

    if (hasInner)
        exceptionInfoResponse.description += "\n Inner exception found, see $exception in variables window for more details.";

    m_evaluator.pop_eval_queue(); // CompleteException
    return S_OK;

failed:
    m_evaluator.pop_eval_queue(); // CompleteException
    IfFailRet(res);
    return res;
}

// MI
HRESULT ManagedDebugger::InsertExceptionBreakpoint(const ExceptionBreakMode &mode,
    const string &name, uint32_t &id)
{
    LogFuncEntry();
    return m_breakpoints.InsertExceptionBreakpoint(mode, name, id);
}

// MI
HRESULT ManagedDebugger::DeleteExceptionBreakpoint(const uint32_t id)
{
    LogFuncEntry();
    return m_breakpoints.DeleteExceptionBreakpoint(id);
}

// MI and VSCode
bool ManagedDebugger::MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const string &exceptionName,
    const ExceptionBreakCategory category)
{
    LogFuncEntry();
    return m_breakpoints.MatchExceptionBreakpoint(dwEventType, exceptionName, category);
}

HRESULT ManagedDebugger::SetEnableCustomNotification(BOOL fEnable)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(m_modules.GetModuleWithName("System.Private.CoreLib.dll", &pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    // in order to make code simple and clear, we don't check enclosing classes with recursion here
    // since we know behaviour for sure, just find "System.Diagnostics.Debugger" first
    mdTypeDef typeDefParent = mdTypeDefNil;
    static const WCHAR strParentTypeDef[] = W("System.Diagnostics.Debugger");
    IfFailRet(pMD->FindTypeDefByName(strParentTypeDef, mdTypeDefNil, &typeDefParent));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDef[] = W("CrossThreadDependencyNotification");
    IfFailRet(pMD->FindTypeDefByName(strTypeDef, typeDefParent, &typeDef));

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pModule->GetClassFromToken(typeDef, &pClass));

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pModule->GetProcess(&pProcess));

    ToRelease<ICorDebugProcess3> pProcess3;
    IfFailRet(pProcess->QueryInterface(IID_ICorDebugProcess3, (LPVOID*) &pProcess3));
    return pProcess3->SetEnableCustomNotification(pClass, fEnable);
}
