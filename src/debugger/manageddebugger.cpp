// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <map>

#include <sys/types.h>
#include <sys/stat.h>

#include "debugger/manageddebugger.h"

#include "valueprint.h"
#include "managed/interop.h"
#include "utils/utf.h"
#include "platform.h"
#include "metadata/typeprinter.h"
#include "debugger/frames.h"
#include "utils/logger.h"

#ifdef FEATURE_PAL
#include <dlfcn.h>
#endif

using std::string;
using std::vector;
using std::map;

namespace netcoredbg
{

#ifdef FEATURE_PAL

// as alternative, libuuid should be linked...
// (the problem is, that in CoreClr > 3.x, in pal/inc/rt/rpc.h,
// MIDL_INTERFACE uses DECLSPEC_UUID, which has empty definition.
extern "C" const IID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }};

namespace {
namespace hook {

class waitpid_t
{
private:
    typedef pid_t (*Signature)(pid_t pid, int *status, int options);
    Signature original = nullptr;
    static constexpr pid_t notConfigured = -1;
    pid_t trackPID = notConfigured;
    int exitCode = 0; // same behaviour as CoreCLR have, by default exit code is 0
    std::recursive_mutex interlock;

    waitpid_t(const waitpid_t&) = delete;
    waitpid_t& operator=(const waitpid_t&) = delete;

    void init() noexcept
    {
        auto ret = dlsym(RTLD_NEXT, "waitpid");
        if (!ret)
        {
            LOGE("Could not find original function waitpid");
            abort();
        }
        original = reinterpret_cast<Signature>(ret);
    }

public:
    waitpid_t() = default;
    ~waitpid_t() = default;

    pid_t operator() (pid_t pid, int *status, int options)
    {
        std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
        if (!original)
        {
            init();
        }
        return original(pid, status, options);
    }

    void SetupTrackingPID(pid_t PID)
    {
        std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
        trackPID = PID;
        exitCode = 0; // same behaviour as CoreCLR have, by default exit code is 0
    }

    int GetExitCode()
    {
        std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
        return exitCode;
    }

    void SetExitCode(pid_t PID, int Code)
    {
        std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
        if (trackPID == notConfigured || PID != trackPID)
        {
            return;
        }
        exitCode = Code;
    }

} waitpid;

}
}

// Note, we guaranty waitpid hook works only during debuggee process execution, it aimed to work only for PAL's waitpid calls interception.
extern "C" pid_t waitpid(pid_t pid, int *status, int options) noexcept
{
    pid_t pidWaitRetval = hook::waitpid(pid, status, options);

    // same logic as PAL have, see PROCGetProcessStatus() and CPalSynchronizationManager::HasProcessExited()
    if (pidWaitRetval == pid)
    {
        if (WIFEXITED(*status))
        {
            hook::waitpid.SetExitCode(pid, WEXITSTATUS(*status));
        }
        else if (WIFSIGNALED(*status))
        {
            LOGW("Process terminated without exiting; can't get exit code. Killed by signal %d. Assuming EXIT_FAILURE.", WTERMSIG(*status));
            hook::waitpid.SetExitCode(pid, EXIT_FAILURE);
        }
    }

    return pidWaitRetval;
}
#endif // FEATURE_PAL


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


namespace
{
    ThreadId getThreadId(ICorDebugThread *pThread)
    {
        DWORD threadId = 0;  // invalid value for Win32
        HRESULT res = pThread->GetID(&threadId);
        return res == S_OK && threadId != 0 ? ThreadId{threadId} : ThreadId{};
    }
}


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
    ThreadId threadId(getThreadId(pThread));

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    m_lastStoppedThreadId = threadId;
}

ThreadId ManagedDebugger::GetLastStoppedThreadId()
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
            ThreadId threadId(getThreadId(pThread));

            if (m_debugger.m_evaluator.IsEvalRunning())
            {
                pAppDomain->Continue(0);
                return S_OK;
            }

            auto stepForcedIgnoreBP = [&] () {
                {
                    std::lock_guard<std::mutex> lock(m_debugger.m_stepMutex);
                    auto stepSettedUpForThread = m_debugger.m_stepSettedUp.find(int(threadId));
                    if (stepSettedUpForThread == m_debugger.m_stepSettedUp.end() || !stepSettedUpForThread->second)
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

                if (atEntry)
                    event.reason = StopEntry;

                ToRelease<ICorDebugFrame> pFrame;
                if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                    m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), event.frame);

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
            ThreadId threadId(getThreadId(pThread));

            StackFrame stackFrame;
            ToRelease<ICorDebugFrame> pFrame;
            HRESULT Status = S_FALSE;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
                Status = m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel(0), stackFrame);

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

            std::lock_guard<std::mutex> lock(m_debugger.m_stepMutex);
            m_debugger.m_stepSettedUp[int(threadId)] = false;

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            LogFuncEntry();
            ThreadId threadId(getThreadId(thread));

            m_debugger.SetLastStoppedThread(thread);

            StoppedEvent event(StopBreak, threadId);

            ToRelease<ICorDebugFrame> pFrame;
            if (SUCCEEDED(thread->GetActiveFrame(&pFrame)) && pFrame != nullptr)
            {
                StackFrame stackFrame;
                if (m_debugger.GetFrameLocation(pFrame, threadId, FrameLevel{0}, stackFrame) == S_OK)
                    event.frame = stackFrame;
            }

            m_debugger.m_protocol->EmitStoppedEvent(event);
            return S_OK;
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

        virtual HRESULT STDMETHODCALLTYPE EvalException(
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
            exitCode = hook::waitpid.GetExitCode();
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

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread)
        {
            LogFuncEntry();
            ThreadId threadId(getThreadId(pThread));
            m_debugger.m_protocol->EmitThreadEvent(ThreadEvent(ThreadStarted, threadId));
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread)
        {
            LogFuncEntry();
            ThreadId threadId(getThreadId(pThread));

            // TODO: clean evaluations and exceptions queues for current thread
            m_debugger.m_evaluator.NotifyEvalComplete(pThread, nullptr);

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
                m_debugger.GetFrameLocation(pActiveFrame, threadId, FrameLevel(0), stackFrame);

            m_debugger.SetLastStoppedThread(pThread);

            event.text = excType;
            event.description = message.empty() ? details : message;
            event.frame = stackFrame;

            if (m_debugger.m_evaluator.IsEvalRunning() && !m_debugger.m_evaluator.is_empty_eval_queue()) {
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

            m_debugger.Stop(threadId, event);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
            /* [in] */ DWORD dwFlags)
        {
            ThreadId threadId(getThreadId(pThread));
            // We produce DEBUG_EXCEPTION_INTERCEPTED from Exception() callback.
            // TODO: we should waiting this unwinding on exit().
            LOGI("ExceptionUnwind:threadId:%d,dwEventType:%d,dwFlags:%d", int(threadId), dwEventType, dwFlags);
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
    m_lastStoppedThreadId(ThreadId::AllThreads),
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
    FrameId::invalidate();

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

HRESULT ManagedDebugger::Launch(const string &fileExec, const vector<string> &execArgs, const map<string, string> &env, const string &cwd, bool stopAtEntry)
{
    LogFuncEntry();

    m_startMethod = StartLaunch;
    m_execPath = fileExec;
    m_execArgs = execArgs;
    m_stopAtEntry = stopAtEntry;
    m_cwd = cwd;
    m_env = env;
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

    ThreadId threadId(getThreadId(pThread));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());

        std::lock_guard<std::mutex> lock(m_stepMutex);
        m_stepSettedUp[int(threadId)] = true;

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

    std::lock_guard<std::mutex> lock(m_stepMutex);
    m_stepSettedUp[int(threadId)] = true;

    return S_OK;
}

HRESULT ManagedDebugger::StepCommand(ThreadId threadId, StepType stepType)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;
    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(int(threadId), &pThread));
    DisableAllSteppers(m_pProcess);
    IfFailRet(SetupStep(pThread, stepType));

    m_variables.Clear();
    Status = m_pProcess->Continue(0);

    if (SUCCEEDED(Status))
    {
        FrameId::invalidate();
        m_protocol->EmitContinuedEvent(threadId);
        --m_stopCounter;
    }
    return Status;
}

HRESULT ManagedDebugger::Stop(ThreadId threadId, const StoppedEvent &event)
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

HRESULT ManagedDebugger::Continue(ThreadId threadId)
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
                    LOGE("Setting thread state failed. Process not synchronized:'%0x'", res);
                break;
                case CORDBG_E_PROCESS_TERMINATED:
                    LOGE("Setting thread state failed. Process was terminated:'%0x'", res);
                break;
                case CORDBG_E_OBJECT_NEUTERED:
                    LOGE("Setting thread state failed. Object has been neutered(it's in a zombie state):'%0x'", res);
                break;
                default:
                    LOGE("SetAllThreadsDebugState() %0x", res);
                break;
            }
        }
    }
    if ((res = m_pProcess->Continue(0)) != S_OK) {
        switch (res) {
        case CORDBG_E_SUPERFLOUS_CONTINUE:
            LOGE("Continue failed. Returned from a call to Continue that was not matched with a stopping event:'%0x'", res);
            break;
        case CORDBG_E_PROCESS_TERMINATED:
            LOGE("Continue failed. Process was terminated:'%0x'", res);
            break;
        case CORDBG_E_OBJECT_NEUTERED:
            LOGE("Continue failed. Object has been neutered(it's in a zombie state):'%0x'", res);
            break;
        default:
            LOGE("Continue() %0x", res);
            break;
        }
    }

    if (SUCCEEDED(res))
    {
        FrameId::invalidate();
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

    ThreadId lastStoppedId = GetLastStoppedThreadId();

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

        if (FAILED(GetStackTrace(thread.id, FrameLevel(0), 0, stackFrames, totalFrames)))
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

    m_protocol->EmitStoppedEvent(StoppedEvent(StopPause, ThreadId::Invalid));

    return Status;
}

HRESULT ManagedDebugger::GetThreads(std::vector<Thread> &threads)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;
    return GetThreadsState(m_pProcess, threads);
}

HRESULT ManagedDebugger::GetStackTrace(ThreadId  threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    HRESULT Status;
    if (!m_pProcess)
        return E_FAIL;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(int(threadId), &pThread));
    return GetStackTrace(pThread, startFrame, maxFrames, stackFrames, totalFrames);
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

    ManagedPart::SetCoreCLRPath(m_clrPath);

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

#ifdef FEATURE_PAL
    hook::waitpid.SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

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

static bool IsDirExists(const char* const path)
{
    struct stat info;

    if (stat(path, &info) != 0)
        return false;

    if (!(info.st_mode & S_IFDIR))
        return false;

    return true;
}

HRESULT ManagedDebugger::RunProcess(const string& fileExec, const std::vector<string>& execArgs)
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

    vector<char> outEnv;
    if (!m_env.empty()) {
        // We need to append the environ values with keeping the current process environment block.
        // It works equal for any platrorms in coreclr CreateProcessW(), but not critical for Linux.
        map<string, string> envMap;
        if (GetSystemEnvironmentAsMap(envMap) != -1) {
            auto it = m_env.begin();
            auto end = m_env.end();
            // Override the system value (PATHs appending needs a complex implementation)
            while (it != end) {
                envMap[it->first] = it->second;
                ++it;
            }
            for (const auto &pair : envMap) {
                outEnv.insert(outEnv.end(), pair.first.begin(), pair.first.end());
                outEnv.push_back('=');
                outEnv.insert(outEnv.end(), pair.second.begin(), pair.second.end());
                outEnv.push_back('\0');
            }
            outEnv.push_back('\0');
        } else {
            for (const auto &pair : m_env) {
                outEnv.insert(outEnv.end(), pair.first.begin(), pair.first.end());
                outEnv.push_back('=');
                outEnv.insert(outEnv.end(), pair.second.begin(), pair.second.end());
                outEnv.push_back('\0');
            }
        }
    }

    // cwd in launch.json set working directory for debugger https://code.visualstudio.com/docs/python/debugging#_cwd
    if (!m_cwd.empty())
        if (!IsDirExists(m_cwd.c_str()) || !SetWorkDir(m_cwd))
            m_cwd.clear();

    IfFailRet(g_dbgshim.CreateProcessForLaunch(reinterpret_cast<LPWSTR>(const_cast<WCHAR*>(to_utf16(ss.str()).c_str())),
                                     /* Suspend process */ TRUE,
                                     outEnv.empty() ? NULL : &outEnv[0],
                                     m_cwd.empty() ? NULL : reinterpret_cast<LPCWSTR>(to_utf16(m_cwd).c_str()),
                                     &m_processId, &resumeHandle));

#ifdef FEATURE_PAL
    hook::waitpid.SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

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

    if (m_startupResult == S_OK)
        m_protocol->EmitExecEvent(PID{m_processId}, fileExec);

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
HRESULT ManagedDebugger::GetExceptionInfoResponse(ThreadId threadId,
    ExceptionInfoResponse &exceptionInfoResponse)
{
    LogFuncEntry();

    // Are needed to move next line to Exception() callback?
    assert(int(threadId) != -1);
    m_evaluator.push_eval_queue(threadId);

    HRESULT res = E_FAIL;
    HRESULT Status = S_OK;
    bool hasInner = false;
    Variable varException;
    ToRelease <ICorDebugValue> evalValue;
    ToRelease<ICorDebugValue> pExceptionValue;
    ToRelease<ICorDebugThread> pThread;

    WCHAR message[] = W("_message\0");
    FrameId frameId;

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

    if ((res = m_pProcess->GetThread(int(threadId), &pThread)) && FAILED(res))
        goto failed;

    if ((res = pThread->GetCurrentException(&pExceptionValue)) && FAILED(res)) {
        LOGE("GetCurrentException() failed, %0x", res);
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

} // namespace netcoredbg
