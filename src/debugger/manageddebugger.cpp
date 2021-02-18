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
#include "debugger/dbgshim.h"
#include "debugger/manageddebugger.h"
#include "debugger/managedcallback.h"


#include "valueprint.h"
#include "managed/interop.h"
#include "utils/utf.h"
#include "platform.h"
#include "metadata/typeprinter.h"
#include "debugger/frames.h"
#include "utils/logger.h"
#include "debugger/waitpid.h"

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

#endif // FEATURE_PAL

dbgshim_t g_dbgshim;

ThreadId getThreadId(ICorDebugThread *pThread)
{
    DWORD threadId = 0;  // invalid value for Win32
    HRESULT res = pThread->GetID(&threadId);
    return res == S_OK && threadId != 0 ? ThreadId{threadId} : ThreadId{};
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
    SetLastStoppedThreadId(getThreadId(pThread));
}

void ManagedDebugger::SetLastStoppedThreadId(ThreadId threadId)
{
    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    m_lastStoppedThreadId = threadId;
}

void ManagedDebugger::InvalidateLastStoppedThreadId()
{
    SetLastStoppedThreadId(ThreadId::AllThreads);
}

ThreadId ManagedDebugger::GetLastStoppedThreadId()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    return m_lastStoppedThreadId;
}

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
            SetLastStoppedThreadId(thread.id);
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
    GetWaitpid().SetupTrackingPID(m_processId);
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
    GetWaitpid().SetupTrackingPID(m_processId);
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

HRESULT ManagedDebugger::SetBreakpoints(
    const std::string& filename,
    const std::vector<SourceBreakpoint> &srcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    return m_breakpoints.SetBreakpoints(m_pProcess, filename, srcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::SetFunctionBreakpoints(
    const std::vector<FunctionBreakpoint> &funcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    return m_breakpoints.SetFunctionBreakpoints(m_pProcess, funcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::GetFrameLocation(ICorDebugFrame *pFrame, ThreadId threadId, FrameLevel level, StackFrame &stackFrame)
{
    HRESULT Status;

    stackFrame = StackFrame(threadId, level, "");

    ULONG32 ilOffset;
    Modules::SequencePoint sp;

    if (SUCCEEDED(m_modules.GetFrameILAndSequencePoint(pFrame, ilOffset, sp)))
    {
        stackFrame.source = Source(sp.document);
        stackFrame.line = sp.startLine;
        stackFrame.column = sp.startColumn;
        stackFrame.endLine = sp.endLine;
        stackFrame.endColumn = sp.endColumn;
    }

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 nOffset = 0;
    ToRelease<ICorDebugNativeFrame> pNativeFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &pNativeFrame));
    IfFailRet(pNativeFrame->GetIP(&nOffset));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    IfFailRet(Modules::GetModuleId(pModule, stackFrame.moduleId));

    stackFrame.clrAddr.methodToken = methodToken;
    stackFrame.clrAddr.ilOffset = ilOffset;
    stackFrame.clrAddr.nativeOffset = nOffset;

    stackFrame.addr = GetFrameAddr(pFrame);

    TypePrinter::GetMethodName(pFrame, stackFrame.name);

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT ManagedDebugger::GetStackTrace(ICorDebugThread *pThread, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    LogFuncEntry();

    HRESULT Status;

    DWORD tid = 0;
    pThread->GetID(&tid);
    ThreadId threadId{tid};

    int currentFrame = -1;

    IfFailRet(WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < int(startFrame))
            return S_OK;
        if (maxFrames != 0 && currentFrame >= int(startFrame) + int(maxFrames))
            return S_OK;

        switch(frameType)
        {
            case FrameUnknown:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, "?");
                stackFrames.back().addr = GetFrameAddr(pFrame);
                break;
            case FrameNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, pNative->symbol);
                stackFrames.back().addr = pNative->addr;
                stackFrames.back().source = Source(pNative->file);
                stackFrames.back().line = pNative->linenum;
                break;
            case FrameCLRNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, "[Native Frame]");
                stackFrames.back().addr = GetFrameAddr(pFrame);
                break;
            case FrameCLRInternal:
                {
                    ToRelease<ICorDebugInternalFrame> pInternalFrame;
                    IfFailRet(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame));
                    CorDebugInternalFrameType corFrameType;
                    IfFailRet(pInternalFrame->GetFrameType(&corFrameType));
                    std::string name = "[";
                    name += GetInternalTypeName(corFrameType);
                    name += "]";
                    stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, name);
                    stackFrames.back().addr = GetFrameAddr(pFrame);
                }
                break;
            case FrameCLRManaged:
                {
                    StackFrame stackFrame;
                    GetFrameLocation(pFrame, threadId, FrameLevel{currentFrame}, stackFrame);
                    stackFrames.push_back(stackFrame);
                }
                break;
        }
        return S_OK;
    }));

    totalFrames = currentFrame + 1;

    return S_OK;
}

int ManagedDebugger::GetNamedVariables(uint32_t variablesReference)
{
    LogFuncEntry();

    return m_variables.GetNamedVariables(variablesReference);
}

HRESULT ManagedDebugger::GetVariables(
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    LogFuncEntry();

    return m_variables.GetVariables(m_pProcess, variablesReference, filter, start, count, variables);
}

HRESULT ManagedDebugger::GetScopes(FrameId frameId, std::vector<Scope> &scopes)
{
    LogFuncEntry();

    return m_variables.GetScopes(m_pProcess, frameId, scopes);
}

HRESULT ManagedDebugger::Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output)
{
    LogFuncEntry();

    return m_variables.Evaluate(m_pProcess, frameId, expression, variable, output);
}

HRESULT ManagedDebugger::SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output)
{
    return m_variables.SetVariable(m_pProcess, name, value, ref, output);
}

HRESULT ManagedDebugger::SetVariableByExpression(
    FrameId frameId,
    const Variable &variable,
    const std::string &value,
    std::string &output)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> pResultValue;

    IfFailRet(m_variables.GetValueByExpression(m_pProcess, frameId, variable, &pResultValue));
    return m_variables.SetVariable(m_pProcess, pResultValue, value, frameId, output);
}

} // namespace netcoredbg
