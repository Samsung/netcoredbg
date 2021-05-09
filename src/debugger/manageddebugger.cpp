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
#include "interfaces/iprotocol.h"
#include "debugger/threads.h"
#include "debugger/frames.h"
#include "debugger/evaluator.h"
#include "debugger/evalwaiter.h"
#include "debugger/variables.h"
#include "debugger/breakpoints.h"
#include "debugger/manageddebugger.h"
#include "debugger/managedcallback.h"
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "valueprint.h"
#include "managed/interop.h"
#include "utils/utf.h"
#include "dynlibs.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "utils/logger.h"
#include "debugger/waitpid.h"
#include "iosystem.h"

#include "palclr.h"

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

namespace 
{
    int GetSystemEnvironmentAsMap(map<string, string>& outMap)
    {
        char*const*const pEnv = GetSystemEnvironment();

        if (pEnv == nullptr)
            return -1;

        int counter = 0;
        while (pEnv[counter] != nullptr)
        {
            const string env = pEnv[counter];
            size_t pos = env.find_first_of("=");
            if (pos != string::npos && pos != 0)
                outMap.emplace(env.substr(0, pos), env.substr(pos+1));

            ++counter;
        }

        return 0;
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

HRESULT ManagedDebugger::DisableAllBreakpointsAndSteppers()
{
    HRESULT Status;

    // Async stepper could have breakpoints active, disable them first.
    IfFailRet(m_uniqueSteppers->DisableAllSteppers(m_iCorProcess));

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(m_iCorProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        ToRelease<ICorDebugBreakpointEnum> breakpoints;
        if (SUCCEEDED(pDomain->EnumerateBreakpoints(&breakpoints)))
        {
            ICorDebugBreakpoint *curBreakpoint;
            ULONG breakpointsFetched;
            while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
            {
                ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
                pBreakpoint->Activate(FALSE);
            }
        }
    }

    return S_OK;
}

void ManagedDebugger::SetLastStoppedThread(ICorDebugThread *pThread)
{
    SetLastStoppedThreadId(getThreadId(pThread));
}

HRESULT ManagedDebugger::GetFullyQualifiedIlOffset(const ThreadId &threadId, FullyQualifiedIlOffset_t &fullyQualifiedIlOffset)
{
    HRESULT Status;

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_iCorProcess->GetThread(int(threadId), &pThread));

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 ilOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    fullyQualifiedIlOffset.modAddress = modAddress;
    fullyQualifiedIlOffset.methodToken = methodToken;
    fullyQualifiedIlOffset.ilOffset = ilOffset;

    return S_OK;
}

void ManagedDebugger::SetLastStoppedThreadId(ThreadId threadId)
{
    std::lock_guard<std::mutex> lock(m_lastStoppedMutex);
    m_lastStoppedThreadId = threadId;

    m_lastStoppedIlOffset.Reset();

    if (m_lastStoppedThreadId == ThreadId::AllThreads)
        return;

    GetFullyQualifiedIlOffset(m_lastStoppedThreadId, m_lastStoppedIlOffset);
}

void ManagedDebugger::InvalidateLastStoppedThreadId()
{
    SetLastStoppedThreadId(ThreadId::AllThreads);
}

ThreadId ManagedDebugger::GetLastStoppedThreadId()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_lastStoppedMutex);
    return m_lastStoppedThreadId;
}

ManagedDebugger::ManagedDebugger() :
    m_processAttachedState(ProcessUnattached),
    m_lastStoppedThreadId(ThreadId::AllThreads),
    m_stopCounter(0),
    m_startMethod(StartNone),
    m_stopAtEntry(false),
    m_isConfigurationDone(false),
    m_sharedThreads(new Threads),
    m_sharedModules(new Modules),
    m_sharedEvalWaiter(new EvalWaiter(m_sharedThreads)),
    m_sharedEvaluator(new Evaluator(m_sharedModules, m_sharedEvalWaiter)),
    m_uniqueSteppers(new Steppers(m_sharedModules, m_sharedEvaluator)),
    m_uniqueBreakpoints(new Breakpoints(m_sharedModules)),
    m_sharedVariables(new Variables(m_sharedEvaluator)),
    m_managedCallback(nullptr),
    m_justMyCode(true),
    m_startupReady(false),
    m_startupResult(S_OK),
    m_unregisterToken(nullptr),
    m_processId(0),
    m_ioredirect(
        { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
        std::bind(&ManagedDebugger::InputCallback, this, std::placeholders::_1, std::placeholders::_2)
    )
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
    m_sharedProtocol->EmitInitializedEvent();
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
    m_uniqueBreakpoints->SetStopAtEntry(m_stopAtEntry);
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
            m_sharedProtocol->EmitTerminatedEvent();

        m_ioredirect.async_cancel();
        return Status;
    }

    return TerminateProcess();
}

HRESULT ManagedDebugger::StepCommand(ThreadId threadId, StepType stepType)
{
    LogFuncEntry();

    if (!m_iCorProcess)
        return E_FAIL;

    if (m_sharedEvalWaiter->IsEvalRunning())
    {
        // Important! Abort all evals before 'Step' in protocol, during eval we have inconsistent thread state.
        LOGE("Can't 'Step' during running evaluation.");
        return E_UNEXPECTED;
    }

    BOOL running = FALSE;
    HRESULT Status;
    IfFailRet(m_iCorProcess->IsRunning(&running));
    if (running)
    {
        LOGW("Can't 'Step', process already running.");
        return E_FAIL;
    }

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_iCorProcess->GetThread(int(threadId), &pThread));
    IfFailRet(m_uniqueSteppers->SetupStep(pThread, stepType));

    if (FAILED(Status = m_iCorProcess->Continue(0)))
        LOGE("Continue failed: %s", errormessage(Status));
    else
    {
        m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
        FrameId::invalidate(); // Clear all created during break frames.
        m_sharedProtocol->EmitContinuedEvent(threadId); // VSCode protocol need thread ID.
    }

    return Status;
}

HRESULT ManagedDebugger::Continue(ThreadId threadId)
{
    LogFuncEntry();

    if (!m_iCorProcess)
        return E_FAIL;

    if (m_sharedEvalWaiter->IsEvalRunning())
    {
        // Important! Abort all evals before 'Continue' in protocol, during eval we have inconsistent thread state.
        LOGE("Can't 'Continue' during running evaluation.");
        return E_UNEXPECTED;
    }

    BOOL running = FALSE;
    HRESULT Status;
    IfFailRet(m_iCorProcess->IsRunning(&running));
    if (running)
    {
        LOGI("Can't 'Continue', process already running.");
        return S_OK; // Send 'OK' response, but don't generate continue event.
    }

    if (FAILED(Status = m_iCorProcess->Continue(0)))
        LOGE("Continue failed: %s", errormessage(Status));
    else
    {
        m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
        FrameId::invalidate(); // Clear all created during break frames.
        m_sharedProtocol->EmitContinuedEvent(threadId); // VSCode protocol need thread ID.
    }

    return Status;
}

HRESULT ManagedDebugger::Pause()
{
    LogFuncEntry();

    if (!m_iCorProcess)
        return E_FAIL;

    // The debugger maintains a stop counter. When the counter goes to zero, the controller is resumed.
    // Each call to Stop or each dispatched callback increments the counter.
    // Each call to ICorDebugController::Continue decrements the counter.
    BOOL running = FALSE;
    HRESULT Status;
    IfFailRet(m_iCorProcess->IsRunning(&running));
    if (!running)
        return S_OK;

    IfFailRet(m_iCorProcess->Stop(0));

    // Same logic as provide vsdbg in case of pause during stepping.
    m_uniqueSteppers->DisableAllSteppers(m_iCorProcess);

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
            m_sharedProtocol->EmitStoppedEvent(event);
            m_ioredirect.async_cancel();
            return Status;
        }
    }

    m_sharedProtocol->EmitStoppedEvent(StoppedEvent(StopPause, ThreadId::Invalid));
    m_ioredirect.async_cancel();
    return Status;
}

HRESULT ManagedDebugger::GetThreads(std::vector<Thread> &threads)
{
    LogFuncEntry();

    if (!m_iCorProcess)
        return E_FAIL;

    return m_sharedThreads->GetThreadsWithState(m_iCorProcess, threads);
}

HRESULT ManagedDebugger::GetStackTrace(ThreadId  threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    LogFuncEntry();

    HRESULT Status;
    if (!m_iCorProcess)
        return E_FAIL;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_iCorProcess->GetThread(int(threadId), &pThread));
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
        self->m_dbgshim.UnregisterForRuntimeStartup(self->m_unregisterToken);
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

static HRESULT InternalEnumerateCLRs(dbgshim_t &dbgshim, DWORD pid, HANDLE **ppHandleArray, LPWSTR **ppStringArray, DWORD *pdwArrayLength, int tryCount)
{
    int numTries = 0;
    HRESULT hr;

    while (numTries < tryCount)
    {
        hr = dbgshim.EnumerateCLRs(pid, ppHandleArray, ppStringArray, pdwArrayLength);

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
                dbgshim.CloseCLREnumeration(*ppHandleArray, *ppStringArray, *pdwArrayLength);

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

static string GetCLRPath(dbgshim_t &dbgshim, DWORD pid, int timeoutSec = 3)
{
    HANDLE* pHandleArray;
    LPWSTR* pStringArray;
    DWORD dwArrayLength;
    const int tryCount = timeoutSec * 10; // 100ms interval between attempts
    if (FAILED(InternalEnumerateCLRs(dbgshim, pid, &pHandleArray, &pStringArray, &dwArrayLength, tryCount)) || dwArrayLength == 0)
        return string();

    string result = to_utf8(pStringArray[0]);

    dbgshim.CloseCLREnumeration(pHandleArray, pStringArray, dwArrayLength);

    return result;
}

HRESULT ManagedDebugger::Startup(IUnknown *punk, DWORD pid)
{
    HRESULT Status;

    ToRelease<ICorDebug> iCorDebug;
    IfFailRet(punk->QueryInterface(IID_ICorDebug, (void **)&iCorDebug));

    IfFailRet(iCorDebug->Initialize());

    if (m_clrPath.empty())
        m_clrPath = GetCLRPath(m_dbgshim, pid);

    // Note, ManagedPart must be initialized before callbacks setup, since callbacks use it.
    // ManagedPart must be initialized only once for process, since CoreCLR don't support unload and reinit
    // for global variables. coreclr_shutdown only should be called on process exit.
    Interop::Init(m_clrPath);

    m_managedCallback.reset(new ManagedCallback(*this));
    Status = iCorDebug->SetManagedHandler(m_managedCallback.get());
    if (FAILED(Status))
    {
        iCorDebug->Terminate();
        m_managedCallback.reset(nullptr);
        return Status;
    }

    ToRelease<ICorDebugProcess> iCorProcess;
    Status = iCorDebug->DebugActiveProcess(pid, FALSE, &iCorProcess);
    if (FAILED(Status))
    {
        iCorDebug->Terminate();
        m_managedCallback.reset(nullptr);
        return Status;
    }

    m_iCorProcess = iCorProcess.Detach();
    m_iCorDebug = iCorDebug.Detach();

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

    HANDLE resumeHandle = 0; // Fake thread handle for the process resume

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

    Status = m_ioredirect.exec([&]() -> HRESULT {
            IfFailRet(m_dbgshim.CreateProcessForLaunch(reinterpret_cast<LPWSTR>(const_cast<WCHAR*>(to_utf16(ss.str()).c_str())),
                                     /* Suspend process */ TRUE,
                                     outEnv.empty() ? NULL : &outEnv[0],
                                     m_cwd.empty() ? NULL : reinterpret_cast<LPCWSTR>(to_utf16(m_cwd).c_str()),
                                     &m_processId, &resumeHandle));
            return Status;
        });

    if (FAILED(Status))
        return Status;

#ifdef FEATURE_PAL
    GetWaitpid().SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

    IfFailRet(m_dbgshim.RegisterForRuntimeStartup(m_processId, ManagedDebugger::StartupCallback, this, &m_unregisterToken));

    // Resume the process so that StartupCallback can run
    IfFailRet(m_dbgshim.ResumeProcess(resumeHandle));
    m_dbgshim.CloseResumeHandle(resumeHandle);

    // Wait for ManagedDebugger::StartupCallback to complete

    /// FIXME: if the process exits too soon the ManagedDebugger::StartupCallback()
    /// is never called (bug in dbgshim?).
    /// The workaround is to wait with timeout.
    const auto now = std::chrono::system_clock::now();

    std::unique_lock<std::mutex> lock(m_startupMutex);
    if (!m_startupCV.wait_until(lock, now + startupCallbackWaitTimeout, [this](){return m_startupReady;}))
    {
        // Timed out
        m_dbgshim.UnregisterForRuntimeStartup(m_unregisterToken);
        m_unregisterToken = nullptr;
        return E_FAIL;
    }

    if (SUCCEEDED(m_startupResult))
        m_sharedProtocol->EmitExecEvent(PID{m_processId}, fileExec);

    return m_startupResult;
}

HRESULT ManagedDebugger::CheckNoProcess()
{
    if (m_iCorProcess || m_iCorDebug)
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
    if (!m_iCorProcess || !m_iCorDebug)
        return E_FAIL;

    if (SUCCEEDED(m_iCorProcess->Stop(0)))
    {
        m_uniqueBreakpoints->DeleteAllBreakpoints();
        DisableAllBreakpointsAndSteppers();
        m_iCorProcess->Detach();
    }

    Cleanup();

    m_iCorProcess.Free();

    m_iCorDebug->Terminate();
    m_iCorDebug.Free();

    if (m_managedCallback->GetRefCount() > 0)
    {
        LOGW("ManagedCallback was not properly released by ICorDebug");
    }
    m_managedCallback.reset(nullptr);

    return S_OK;
}

HRESULT ManagedDebugger::TerminateProcess()
{
    if (!m_iCorProcess || !m_iCorDebug)
        return E_FAIL;

    if (SUCCEEDED(m_iCorProcess->Stop(0)))
    {
        DisableAllBreakpointsAndSteppers();
        //pProcess->Detach();
    }

    Cleanup();

    m_iCorProcess->Terminate(0);
    WaitProcessExited();

    m_iCorProcess.Free();

    m_iCorDebug->Terminate();
    m_iCorDebug.Free();

    if (m_managedCallback->GetRefCount() > 0)
    {
        LOGW("ManagedCallback was not properly released by ICorDebug");
    }
    m_managedCallback.reset(nullptr);

    return S_OK;
}

void ManagedDebugger::Cleanup()
{
    m_sharedModules->CleanupAllModules();
    m_sharedEvaluator->Cleanup();
    m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
    m_sharedProtocol->Cleanup();
}

HRESULT ManagedDebugger::AttachToProcess(DWORD pid)
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    m_clrPath = GetCLRPath(m_dbgshim, pid);
    if (m_clrPath.empty())
        return E_INVALIDARG; // Unable to find libcoreclr.so

    WCHAR pBuffer[100];
    DWORD dwLength;
    IfFailRet(m_dbgshim.CreateVersionStringFromModule(
        pid,
        reinterpret_cast<LPCWSTR>(to_utf16(m_clrPath).c_str()),
        pBuffer,
        _countof(pBuffer),
        &dwLength));

    ToRelease<IUnknown> pCordb;

    IfFailRet(m_dbgshim.CreateDebuggingInterfaceFromVersionEx(CorDebugVersion_4_0, pBuffer, &pCordb));

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

        if ((res = m_uniqueBreakpoints->GetExceptionBreakMode(mode, "*")) && FAILED(res))
            goto failed;

        exceptionInfoResponse.breakMode = mode;
    }

    if ((res = m_iCorProcess->GetThread(int(threadId), &pThread)) && FAILED(res))
        goto failed;

    if ((res = pThread->GetCurrentException(&pExceptionValue)) && FAILED(res)) {
        LOGE("GetCurrentException() failed, %0x", res);
        goto failed;
    }

    PrintStringField(pExceptionValue, message, exceptionInfoResponse.description);

    if ((res = m_sharedVariables->GetExceptionVariable(frameId, pThread, varException)) && FAILED(res))
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

    if (FAILED(m_sharedEvaluator->getObjectByFunction("get_StackTrace", pThread, pExceptionValue, &evalValue, defaultEvalFlags))) {
        exceptionInfoResponse.details.stackTrace = "<undefined>"; // Evaluating problem entire object
    }
    else {
        PrintValue(evalValue, exceptionInfoResponse.details.stackTrace);
        ToRelease <ICorDebugValue> evalValueOut;
        BOOL isNotNull = TRUE;

        ICorDebugValue *evalValueInner = pExceptionValue;
        while (isNotNull) {
            if ((res = m_sharedEvaluator->getObjectByFunction("get_InnerException", pThread, evalValueInner, &evalValueOut, defaultEvalFlags)) && FAILED(res))
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

            if ((res = m_sharedEvaluator->getObjectByFunction("get_StackTrace", pThread, evalValueOut, &pValueTmp, defaultEvalFlags)) && FAILED(res))
                goto failed;

            PrintValue(pValueTmp, inner.stackTrace);

            exceptionInfoResponse.details.innerException.push_back(inner);
            evalValueInner = evalValueOut;
        }
    }

    if (hasInner)
        exceptionInfoResponse.description += "\n Inner exception found, see $exception in variables window for more details.";

    return S_OK;

failed:
    IfFailRet(res);
    return res;
}

// MI
HRESULT ManagedDebugger::InsertExceptionBreakpoint(const ExceptionBreakMode &mode,
    const string &name, uint32_t &id)
{
    LogFuncEntry();
    return m_uniqueBreakpoints->InsertExceptionBreakpoint(mode, name, id);
}

// MI
HRESULT ManagedDebugger::DeleteExceptionBreakpoint(const uint32_t id)
{
    LogFuncEntry();
    return m_uniqueBreakpoints->DeleteExceptionBreakpoint(id);
}

// MI and VSCode
bool ManagedDebugger::MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const string &exceptionName,
    const ExceptionBreakCategory category)
{
    return m_uniqueBreakpoints->MatchExceptionBreakpoint(dwEventType, exceptionName, category);
}

HRESULT ManagedDebugger::SetEnableCustomNotification(BOOL fEnable)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(m_sharedModules->GetModuleWithName("System.Private.CoreLib.dll", &pModule));

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

    return m_uniqueBreakpoints->SetBreakpoints(m_iCorProcess, filename, srcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::SetFunctionBreakpoints(
    const std::vector<FunctionBreakpoint> &funcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    return m_uniqueBreakpoints->SetFunctionBreakpoints(m_iCorProcess, funcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::BreakpointActivate(int id, bool act)
{
    return m_uniqueBreakpoints->BreakpointActivate(id, act);
}

HRESULT ManagedDebugger::AllBreakpointsActivate(bool act)
{
    return m_uniqueBreakpoints->AllBreakpointsActivate(act);
}

HRESULT ManagedDebugger::GetFrameLocation(ICorDebugFrame *pFrame, ThreadId threadId, FrameLevel level, StackFrame &stackFrame)
{
    HRESULT Status;

    stackFrame = StackFrame(threadId, level, "");

    ULONG32 ilOffset;
    Modules::SequencePoint sp;

    if (SUCCEEDED(m_sharedModules->GetFrameILAndSequencePoint(pFrame, ilOffset, sp)))
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

    return S_OK;
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

    return m_sharedVariables->GetNamedVariables(variablesReference);
}

HRESULT ManagedDebugger::GetVariables(
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    LogFuncEntry();

    return m_sharedVariables->GetVariables(m_iCorProcess, variablesReference, filter, start, count, variables);
}

HRESULT ManagedDebugger::GetScopes(FrameId frameId, std::vector<Scope> &scopes)
{
    LogFuncEntry();

    return m_sharedVariables->GetScopes(m_iCorProcess, frameId, scopes);
}

HRESULT ManagedDebugger::Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output)
{
    LogFuncEntry();

    return m_sharedVariables->Evaluate(m_iCorProcess, frameId, expression, variable, output);
}

HRESULT ManagedDebugger::SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output)
{
    return m_sharedVariables->SetVariable(m_iCorProcess, name, value, ref, output);
}

HRESULT ManagedDebugger::SetVariableByExpression(
    FrameId frameId,
    const Variable &variable,
    const std::string &value,
    std::string &output)
{
    LogFuncEntry();

    HRESULT Status;
    ToRelease<ICorDebugValue> pResultValue;

    IfFailRet(m_sharedVariables->GetValueByExpression(m_iCorProcess, frameId, variable, &pResultValue));
    return m_sharedVariables->SetVariable(m_iCorProcess, pResultValue, value, frameId, output);
}


void ManagedDebugger::FindFileNames(string_view pattern, unsigned limit, SearchCallback cb)
{
    LogFuncEntry();
    m_sharedModules->FindFileNames(pattern, limit, cb);
}

void ManagedDebugger::FindFunctions(string_view pattern, unsigned limit, SearchCallback cb)
{
    LogFuncEntry();
    m_sharedModules->FindFunctions(pattern, limit, cb);
}

void ManagedDebugger::FindVariables(ThreadId thread, FrameLevel framelevel, string_view pattern, unsigned limit, SearchCallback cb)
{
    LogFuncEntry();
    StackFrame frame{thread, framelevel, ""};
    std::vector<Scope> scopes;
    std::vector<Variable> variables;
    HRESULT status = GetScopes(frame.id, scopes);
    if (FAILED(status))
    {
        LOGW("GetScopes failed: %s", errormessage(status));
        return;
    }

    if (scopes.empty() || scopes[0].variablesReference == 0)
    {
        LOGW("no variables in visible scopes");
        return;
    }

    status = GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables);
    if (FAILED(status))
    {
        LOGW("GetVariables failed: %s", errormessage(status));
        return;
    }

    for (const Variable& var : variables)
    {
        LOGD("var: '%s'", var.name.c_str());

        if (limit == 0)
            break;

        auto pos = var.name.find(pattern.data(), 0, pattern.size());
        if (pos != std::string::npos && (pos == 0 || var.name[pos-1] == '.'))
        {
            limit--;
            cb(var.name.c_str());
        }
    }
}


void ManagedDebugger::InputCallback(IORedirectHelper::StreamType type, span<char> text)
{
    m_sharedProtocol->EmitOutputEvent(type == IOSystem::Stderr ? OutputStdErr : OutputStdOut, {text.begin(), text.size()});
}


void ManagedDebugger::EnumerateBreakpoints(std::function<bool (const BreakpointInfo&)>&& callback)
{
    LogFuncEntry();
    return m_uniqueBreakpoints->EnumerateBreakpoints(std::move(callback));
}


IDebugger::AsyncResult ManagedDebugger::ProcessStdin(InStream& stream)
{
    LogFuncEntry();
    return m_ioredirect.async_input(stream);
}


void ManagedDebugger::SetJustMyCode(bool enable)
{
    m_justMyCode = enable;
    m_uniqueSteppers->SetJustMyCode(enable);
}

} // namespace netcoredbg
