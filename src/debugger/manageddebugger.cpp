// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <map>
#include <fstream>

#include <sys/types.h>
#include <sys/stat.h>
#include "interfaces/iprotocol.h"
#include "debugger/threads.h"
#include "debugger/frames.h"
#include "debugger/evalhelpers.h"
#include "debugger/evalstackmachine.h"
#include "debugger/evaluator.h"
#include "debugger/evalwaiter.h"
#include "debugger/variables.h"
#include "debugger/breakpoint_break.h"
#include "debugger/breakpoint_entry.h"
#include "debugger/breakpoints_exception.h"
#include "debugger/breakpoints_func.h"
#include "debugger/breakpoints_line.h"
#include "debugger/breakpoint_hotreload.h"
#include "debugger/breakpoint_interop_rendezvous.h"
#include "debugger/breakpoints_interop.h"
#include "debugger/breakpoints_interop_line.h"
#include "debugger/breakpoints.h"
#include "debugger/hotreloadhelpers.h"
#include "debugger/manageddebugger.h"
#include "debugger/managedcallback.h"
#include "debugger/callbacksqueue.h"
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "managed/interop.h"
#include "metadata/interop_libraries.h"
#include "utils/utf.h"
#include "utils/dynlibs.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "utils/logger.h"
#include "debugger/waitpid.h"
#include "utils/iosystem.h"

#ifdef INTEROP_DEBUGGING
#include "elf++.h"
#include "dwarf++.h"
#include "debugger/sigaction.h"
#endif // INTEROP_DEBUGGING

#include "palclr.h"

namespace netcoredbg
{

#ifdef FEATURE_PAL

// as alternative, libuuid should be linked...
// the problem is, that in CoreClr > 3.x, in pal/inc/rt/rpc.h,
// MIDL_INTERFACE uses DECLSPEC_UUID, which has empty definition.
extern "C" const IID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }};

#endif // FEATURE_PAL

namespace
{
    const auto startupWaitTimeout = std::chrono::milliseconds(5000);

    const std::string envDOTNET_STARTUP_HOOKS = "DOTNET_STARTUP_HOOKS";
#ifdef FEATURE_PAL
    const char delimiterDOTNET_STARTUP_HOOKS = ':';
#else  // FEATURE_PAL
    const char delimiterDOTNET_STARTUP_HOOKS = ';';
#endif // FEATURE_PAL

#ifdef INTEROP_DEBUGGING
    const std::string envNCDB_INTEROP_DEBUGGING = "NCDB_INTEROP_DEBUGGING";
#endif // INTEROP_DEBUGGING

    int GetSystemEnvironmentAsMap(std::map<std::string, std::string>& outMap)
    {
        char*const*const pEnv = GetSystemEnvironment();

        if (pEnv == nullptr)
            return -1;

        int counter = 0;
        while (pEnv[counter] != nullptr)
        {
            const std::string env = pEnv[counter];
            size_t pos = env.find_first_of("=");
            if (pos != std::string::npos && pos != 0)
                outMap.emplace(env.substr(0, pos), env.substr(pos+1));

            ++counter;
        }

        return 0;
    }
}

// Caller must care about m_debugProcessRWLock.
HRESULT ManagedDebuggerBase::CheckDebugProcess()
{
    if (!m_iCorProcess)
        return E_FAIL;

    // We might have case, when process was exited/detached, but m_iCorProcess still not free and hold invalid object.
    // Note, we can't hold this lock, since this could deadlock execution at ICorDebugManagedCallback::ExitProcess call.
    std::unique_lock<std::mutex> lockAttachedMutex(m_processAttachedMutex);
    if (m_processAttachedState == ProcessAttachedState::Unattached)
        return E_FAIL;
    lockAttachedMutex.unlock();

    return S_OK;
}

bool ManagedDebuggerBase::HaveDebugProcess()
{
    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    return SUCCEEDED(CheckDebugProcess());
}

void ManagedDebuggerBase::NotifyProcessCreated()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessAttachedState::Attached;
    lock.unlock();
    m_processAttachedCV.notify_one();
}

void ManagedDebuggerBase::NotifyProcessExited()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessAttachedState::Unattached;
    lock.unlock();
    m_processAttachedCV.notify_all();
}

// Caller must care about m_debugProcessRWLock.
void ManagedDebuggerBase::DisableAllBreakpointsAndSteppers()
{
    m_uniqueSteppers->DisableAllSteppers(m_iCorProcess); // Async stepper could have breakpoints active, disable them first.
    m_sharedBreakpoints->DeleteAllManaged();
    m_sharedBreakpoints->DisableAllManaged(m_iCorProcess); // Last one, disable all breakpoints on all domains, even if we don't hold them.
}

void ManagedDebuggerBase::SetLastStoppedThread(ICorDebugThread *pThread)
{
    SetLastStoppedThreadId(getThreadId(pThread));
}

void ManagedDebuggerBase::SetLastStoppedThreadId(ThreadId threadId)
{
    std::lock_guard<std::mutex> lock(m_lastStoppedMutex);
    m_lastStoppedThreadId = threadId;

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);

    m_sharedBreakpoints->SetLastStoppedIlOffset(m_iCorProcess, m_lastStoppedThreadId);
}

void ManagedDebuggerBase::InvalidateLastStoppedThreadId()
{
    SetLastStoppedThreadId(ThreadId::AllThreads);
}

ThreadId ManagedDebugger::GetLastStoppedThreadId()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_lastStoppedMutex);
    return m_lastStoppedThreadId;
}

ManagedDebuggerBase::ManagedDebuggerBase(IProtocol *pProtocol_) :
    m_processAttachedState(ProcessAttachedState::Unattached),
    m_lastStoppedThreadId(ThreadId::AllThreads),
    m_startMethod(StartNone),
    m_isConfigurationDone(false),
    pProtocol(pProtocol_),
    m_sharedThreads(new Threads),
    m_sharedModules(new Modules),
    m_sharedEvalWaiter(new EvalWaiter),
    m_sharedEvalHelpers(new EvalHelpers(m_sharedModules, m_sharedEvalWaiter)),
    m_sharedEvalStackMachine(new EvalStackMachine),
    m_sharedEvaluator(new Evaluator(m_sharedModules, m_sharedEvalHelpers, m_sharedEvalStackMachine)),
    m_sharedVariables(new Variables(m_sharedEvalHelpers, m_sharedEvaluator, m_sharedEvalStackMachine)),
    m_uniqueSteppers(new Steppers(m_sharedModules, m_sharedEvalHelpers)),
    m_sharedBreakpoints(new Breakpoints(m_sharedModules, m_sharedEvaluator, m_sharedEvalHelpers, m_sharedVariables)),
    m_sharedCallbacksQueue(nullptr),
    m_uniqueManagedCallback(nullptr),
#ifdef INTEROP_DEBUGGING
    m_sharedInteropDebugger(new InteropDebugging::InteropDebugger(pProtocol, m_sharedBreakpoints, m_sharedEvalWaiter)),
#endif // INTEROP_DEBUGGING
    m_justMyCode(true),
    m_stepFiltering(true),
    m_hotReload(false),
    m_interopDebugging(false),
    m_unregisterToken(nullptr),
    m_processId(0),
    m_ioredirect(
        { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
        std::bind(&ManagedDebugger::InputCallback, this, std::placeholders::_1, std::placeholders::_2)
    )
{
    m_sharedEvalStackMachine->SetupEval(m_sharedEvaluator, m_sharedEvalHelpers, m_sharedEvalWaiter);
    m_sharedThreads->SetEvaluator(m_sharedEvaluator);
#ifdef INTEROP_DEBUGGING
    // Note, we don't care about m_interopDebugging here, since m_interopDebugging could be changed with env parsing before real start/attach.
    m_sharedEvalWaiter->SetInteropDebugger(m_sharedInteropDebugger);
#endif // INTEROP_DEBUGGING
}

ManagedDebuggerHelpers::ManagedDebuggerHelpers(IProtocol *pProtocol_) :
    ManagedDebuggerBase(pProtocol_)
{}

ManagedDebugger::ManagedDebugger(IProtocol *pProtocol_) :
    ManagedDebuggerHelpers(pProtocol_)
{}

ManagedDebuggerBase::~ManagedDebuggerBase()
{
#ifdef INTEROP_DEBUGGING
    // Note, we don't care about m_interopDebugging here, since m_interopDebugging could be changed with env parsing before real start/attach.
    m_sharedEvalWaiter->ResetInteropDebugger();
#endif // INTEROP_DEBUGGING
    m_sharedThreads->ResetEvaluator();
    m_sharedEvalStackMachine->ResetEval();
}

HRESULT ManagedDebugger::Initialize()
{
    LogFuncEntry();

    // TODO: Report capabilities and check client support
    m_startMethod = StartNone;
    return S_OK;
}

HRESULT ManagedDebuggerHelpers::RunIfReady()
{
    FrameId::invalidate();

    if (m_startMethod == StartNone || !m_isConfigurationDone)
        return S_OK;

    switch(m_startMethod)
    {
        case StartLaunch:
            return RunProcess(m_execPath, m_execArgs);
        case StartAttach:
            return AttachToProcess();
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

HRESULT ManagedDebugger::Launch(const std::string &fileExec, const std::vector<std::string> &execArgs,
                                const std::map<std::string, std::string> &env, const std::string &cwd, bool stopAtEntry)
{
    LogFuncEntry();

    m_startMethod = StartLaunch;
    m_execPath = fileExec;
    m_execArgs = execArgs;
    m_cwd = cwd;
    m_env = env;
    m_sharedBreakpoints->SetStopAtEntry(stopAtEntry);
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
            if (m_startMethod != StartAttach)
            {
                LOGE("Can't detach debugger form child process.\n");
                return E_INVALIDARG;
            }
            terminate = false;
            break;
        default:
            return E_FAIL;
    }

#ifdef INTEROP_DEBUGGING
    if (m_interopDebugging)
        m_sharedInteropDebugger->Shutdown();
#endif

    if (!terminate)
    {
        HRESULT Status = DetachFromProcess();
        if (SUCCEEDED(Status))
            pProtocol->EmitTerminatedEvent();

        m_ioredirect.async_cancel();
        return Status;
    }

    return TerminateProcess();
}

HRESULT ManagedDebugger::StepCommand(ThreadId threadId, StepType stepType)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    if (m_sharedEvalWaiter->IsEvalRunning())
    {
        // Important! Abort all evals before 'Step' in protocol, during eval we have inconsistent thread state.
        LOGE("Can't 'Step' during running evaluation.");
        return E_UNEXPECTED;
    }

    if (m_sharedCallbacksQueue->IsRunning())
    {
        LOGW("Can't 'Step', process already running.");
        return E_FAIL;
    }

    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_iCorProcess->GetThread(int(threadId), &pThread));
    IfFailRet(m_uniqueSteppers->SetupStep(pThread, stepType));

    m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
    FrameId::invalidate(); // Clear all created during break frames.
    pProtocol->EmitContinuedEvent(threadId); // VSCode protocol need thread ID.

    // Note, process continue must be after event emitted, since we could get new stop event from queue here.
    if (FAILED(Status = m_sharedCallbacksQueue->Continue(m_iCorProcess)))
        LOGE("Continue failed: %s", errormessage(Status));

    return Status;
}

HRESULT ManagedDebugger::Continue(ThreadId threadId)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    if (m_sharedEvalWaiter->IsEvalRunning())
    {
        // Important! Abort all evals before 'Continue' in protocol, during eval we have inconsistent thread state.
        LOGE("Can't 'Continue' during running evaluation.");
        return E_UNEXPECTED;
    }

    if (m_sharedCallbacksQueue->IsRunning())
    {
        LOGI("Can't 'Continue', process already running.");
        return S_OK; // Send 'OK' response, but don't generate continue event.
    }

    m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
    FrameId::invalidate(); // Clear all created during break frames.
    pProtocol->EmitContinuedEvent(threadId); // VSCode protocol need thread ID.

    // Note, process continue must be after event emitted, since we could get new stop event from queue here.
    if (FAILED(Status = m_sharedCallbacksQueue->Continue(m_iCorProcess)))
        LOGE("Continue failed: %s", errormessage(Status));

    return Status;
}

HRESULT ManagedDebugger::Pause(ThreadId lastStoppedThread, EventFormat eventFormat)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedCallbacksQueue->Pause(m_iCorProcess, lastStoppedThread, eventFormat);
}

HRESULT ManagedDebugger::GetThreads(std::vector<Thread> &threads, bool withNativeThreads)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

#ifdef INTEROP_DEBUGGING
    if (m_interopDebugging && withNativeThreads)
        return m_sharedThreads->GetInteropThreadsWithState(m_iCorProcess, m_sharedInteropDebugger.get(), threads);
#endif // INTEROP_DEBUGGING
    return m_sharedThreads->GetThreadsWithState(m_iCorProcess, threads);
}

VOID ManagedDebuggerHelpers::StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr)
{
    ManagedDebugger *self = static_cast<ManagedDebugger*>(parameter);

    self->Startup(pCordb);

    if (self->m_unregisterToken)
    {
        self->m_dbgshim.UnregisterForRuntimeStartup(self->m_unregisterToken);
        self->m_unregisterToken = nullptr;
    }
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

static HRESULT EnumerateCLRs(dbgshim_t &dbgshim, DWORD pid, HANDLE **ppHandleArray, LPWSTR **ppStringArray, DWORD *pdwArrayLength, int tryCount)
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

static std::string GetCLRPath(dbgshim_t &dbgshim, DWORD pid, int timeoutSec = 3)
{
    HANDLE* pHandleArray;
    LPWSTR* pStringArray;
    DWORD dwArrayLength;
    const int tryCount = timeoutSec * 10; // 100ms interval between attempts
    if (FAILED(EnumerateCLRs(dbgshim, pid, &pHandleArray, &pStringArray, &dwArrayLength, tryCount)) || dwArrayLength == 0)
        return std::string();

    std::string result = to_utf8(pStringArray[0]);

    dbgshim.CloseCLREnumeration(pHandleArray, pStringArray, dwArrayLength);

    return result;
}

HRESULT ManagedDebuggerHelpers::Startup(IUnknown *punk)
{
    HRESULT Status;

    ToRelease<ICorDebug> iCorDebug;
    IfFailRet(punk->QueryInterface(IID_ICorDebug, (void **)&iCorDebug));

    IfFailRet(iCorDebug->Initialize());

    if (m_clrPath.empty())
        m_clrPath = GetCLRPath(m_dbgshim, m_processId);

    m_sharedCallbacksQueue.reset(new CallbacksQueue(*this));
    m_uniqueManagedCallback.reset(new ManagedCallback(*this, m_sharedCallbacksQueue));
    Status = iCorDebug->SetManagedHandler(m_uniqueManagedCallback.get());
    if (FAILED(Status))
    {
        iCorDebug->Terminate();
        m_uniqueManagedCallback.reset();
        m_sharedCallbacksQueue.reset();
        return Status;
    }

    ToRelease<ICorDebugProcess> iCorProcess;
    Status = iCorDebug->DebugActiveProcess(m_processId, FALSE, &iCorProcess);
    if (FAILED(Status))
    {
        iCorDebug->Terminate();
        m_uniqueManagedCallback.reset();
        m_sharedCallbacksQueue.reset();
        return Status;
    }

    std::unique_lock<Utility::RWLock::Writer> lockProcessRWLock(m_debugProcessRWLock.writer);

    m_iCorProcess = iCorProcess.Detach();
    m_iCorDebug = iCorDebug.Detach();

    lockProcessRWLock.unlock();

#ifdef FEATURE_PAL
    GetWaitpid().SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

    return S_OK;
}

static std::string EscapeShellArg(const std::string &arg)
{
    std::string s(arg);

    for (std::string::size_type i = 0; i < s.size(); ++i)
    {
        std::string::size_type count = 0;
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

static void SetCustomEnvironmentArgs(std::map<std::string, std::string> &env, bool hotReload)
{
#ifdef NCDB_DOTNET_STARTUP_HOOK
    if (hotReload)
    {
        auto find = env.find(envDOTNET_STARTUP_HOOKS);
        if (find != env.end())
            find->second = find->second + delimiterDOTNET_STARTUP_HOOKS + NCDB_DOTNET_STARTUP_HOOK;
        else
            env[envDOTNET_STARTUP_HOOKS] = NCDB_DOTNET_STARTUP_HOOK;
    }
#else
    (void)hotReload; // suppress warning about unused param
#endif // NCDB_DOTNET_STARTUP_HOOK
}

static void PrepareSystemEnvironmentArg(const std::map<std::string, std::string> &env, std::vector<char> &outEnv, bool hotReload, bool &interopDebugging)
{
    // We need to append the environ values with keeping the current process environment block.
    // It works equal for any platrorms in coreclr CreateProcessW(), but not critical for Linux.
    std::map<std::string, std::string> envMap;
    if (GetSystemEnvironmentAsMap(envMap) != -1)
    {
        // Override the system value (PATHs appending needs a complex implementation)
        for (const auto &pair : env)
        {
            if (pair.first == envDOTNET_STARTUP_HOOKS && !envMap[pair.first].empty())
            {
                envMap[pair.first] = envMap[pair.first] + delimiterDOTNET_STARTUP_HOOKS + pair.second;
                continue;
            }
#ifdef INTEROP_DEBUGGING
            else if (pair.first == envNCDB_INTEROP_DEBUGGING)
            {
                interopDebugging = true;
                continue;
            }
#else
            (void)interopDebugging; // suppress warning about unused param
#endif // INTEROP_DEBUGGING
            envMap[pair.first] = pair.second;
        }
    }
    else
    {
        envMap = env;
    }
    SetCustomEnvironmentArgs(envMap, hotReload);
    for (const auto &pair : envMap)
    {
        outEnv.insert(outEnv.end(), pair.first.begin(), pair.first.end());
        outEnv.push_back('=');
        outEnv.insert(outEnv.end(), pair.second.begin(), pair.second.end());
        outEnv.push_back('\0');
    }
    // Environtment variable should looks like: "Var=Value\0OtherVar=OtherValue\0\0"
    if (!outEnv.empty())
        outEnv.push_back('\0');
}

HRESULT ManagedDebuggerHelpers::RunProcess(const std::string& fileExec, const std::vector<std::string>& execArgs)
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    std::ostringstream ss;
    ss << "\"" << fileExec << "\"";
    for (const std::string &arg : execArgs)
    {
        ss << " \"" << EscapeShellArg(arg) << "\"";
    }

    m_clrPath.clear();

    HANDLE resumeHandle = 0; // Fake thread handle for the process resume

#ifdef INTEROP_DEBUGGING
    bool prevInteropDebuggingStatus = !!m_interopDebugging;
#endif INTEROP_DEBUGGING

    std::vector<char> outEnv;
    PrepareSystemEnvironmentArg(m_env, outEnv, m_hotReload, m_interopDebugging);

#ifdef INTEROP_DEBUGGING
    if ((!!m_interopDebugging) != prevInteropDebuggingStatus)
    {
        // In case of interop debugging we depend on SIGCHLD set to SIG_DFL by init code.
        // Note, debugger include corhost (CoreCLR) that could setup sigaction for SIGCHLD and ruin interop debugger work.
        SetSigactionMode(!!m_interopDebugging);
    }
#endif INTEROP_DEBUGGING

    // cwd in launch.json set working directory for debugger https://code.visualstudio.com/docs/python/debugging#_cwd
    if (!m_cwd.empty())
    {
        if (!IsDirExists(m_cwd.c_str()) || !SetWorkDir(m_cwd))
            m_cwd.clear();
    }

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

    std::unique_lock<std::mutex> lockAttachedMutex(m_processAttachedMutex);
    if (!m_processAttachedCV.wait_for(lockAttachedMutex, startupWaitTimeout, [this]{return m_processAttachedState == ProcessAttachedState::Attached;}))
        return E_FAIL;

    pProtocol->EmitExecEvent(PID{m_processId}, fileExec);

    return S_OK;
}

HRESULT ManagedDebuggerBase::CheckNoProcess()
{
    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);

    if (!m_iCorProcess)
        return S_OK;

    std::unique_lock<std::mutex> lockAttachedMutex(m_processAttachedMutex);
    if (m_processAttachedState == ProcessAttachedState::Attached)
        return E_FAIL; // Already attached
    lockAttachedMutex.unlock();

    Cleanup();
    return S_OK;
}

HRESULT ManagedDebuggerHelpers::DetachFromProcess()
{
    do {
        std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
        std::lock_guard<std::mutex> guardAttachedMutex(m_processAttachedMutex);
        if (m_processAttachedState == ProcessAttachedState::Unattached)
            break;

        if (!m_iCorProcess)
            return E_FAIL;

        BOOL procRunning = FALSE;
        if (SUCCEEDED(m_iCorProcess->IsRunning(&procRunning)) && procRunning == TRUE)
            m_iCorProcess->Stop(0);

        DisableAllBreakpointsAndSteppers();

        HRESULT Status;
        if (FAILED(Status = m_iCorProcess->Detach()))
            LOGE("Process detach failed: %s", errormessage(Status));

        m_processAttachedState = ProcessAttachedState::Unattached; // Since we free process object anyway, reset process attached state.
    } while(0);

    Cleanup();
    return S_OK;
}

HRESULT ManagedDebuggerHelpers::TerminateProcess()
{
    do {
        std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
        std::unique_lock<std::mutex> lockAttachedMutex(m_processAttachedMutex);
        if (m_processAttachedState == ProcessAttachedState::Unattached)
            break;

        if (!m_iCorProcess)
            return E_FAIL;

        BOOL procRunning = FALSE;
        if (SUCCEEDED(m_iCorProcess->IsRunning(&procRunning)) && procRunning == TRUE)
            m_iCorProcess->Stop(0);

        DisableAllBreakpointsAndSteppers();

        HRESULT Status;
        if (SUCCEEDED(Status = m_iCorProcess->Terminate(0)))
        {
            m_processAttachedCV.wait(lockAttachedMutex, [this]{return m_processAttachedState == ProcessAttachedState::Unattached;});
            break;
        }

        LOGE("Process terminate failed: %s", errormessage(Status));
        m_processAttachedState = ProcessAttachedState::Unattached; // Since we free process object anyway, reset process attached state.
    } while(0);

    Cleanup();
    return S_OK;
}

void ManagedDebuggerBase::Cleanup()
{
    m_sharedModules->CleanupAllModules();
    m_sharedEvalHelpers->Cleanup();
    m_sharedVariables->Clear(); // Important, must be sync with MIProtocol m_vars.clear()
    pProtocol->Cleanup();

    std::lock_guard<Utility::RWLock::Writer> guardProcessRWLock(m_debugProcessRWLock.writer);

    assert((m_iCorProcess && m_iCorDebug && m_uniqueManagedCallback && m_sharedCallbacksQueue) ||
           (!m_iCorProcess && !m_iCorDebug && !m_uniqueManagedCallback && !m_sharedCallbacksQueue));

    if (!m_iCorProcess)
        return;

    m_iCorProcess.Free();

    m_iCorDebug->Terminate();
    m_iCorDebug.Free();

    if (m_uniqueManagedCallback->GetRefCount() > 0)
    {
        LOGW("ManagedCallback was not properly released by ICorDebug");
    }
    m_uniqueManagedCallback.reset(nullptr);
    m_sharedCallbacksQueue = nullptr;
}

HRESULT ManagedDebuggerHelpers::AttachToProcess()
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    m_clrPath = GetCLRPath(m_dbgshim, m_processId);
    if (m_clrPath.empty())
        return E_INVALIDARG; // Unable to find libcoreclr.so

    WCHAR pBuffer[100];
    DWORD dwLength;
    IfFailRet(m_dbgshim.CreateVersionStringFromModule(
        m_processId,
        reinterpret_cast<LPCWSTR>(to_utf16(m_clrPath).c_str()),
        pBuffer,
        _countof(pBuffer),
        &dwLength));

    ToRelease<IUnknown> pCordb;

    IfFailRet(m_dbgshim.CreateDebuggingInterfaceFromVersionEx(CorDebugVersion_4_0, pBuffer, &pCordb));

    m_unregisterToken = nullptr;
    IfFailRet(Startup(pCordb));

    std::unique_lock<std::mutex> lockAttachedMutex(m_processAttachedMutex);
    if (!m_processAttachedCV.wait_for(lockAttachedMutex, startupWaitTimeout, [this]{return m_processAttachedState == ProcessAttachedState::Attached;}))
        return E_FAIL;

    return S_OK;
}

HRESULT ManagedDebugger::GetExceptionInfo(ThreadId threadId, ExceptionInfo &exceptionInfo)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    ToRelease<ICorDebugThread> iCorThread;
    IfFailRet(m_iCorProcess->GetThread(int(threadId), &iCorThread));
    return m_sharedBreakpoints->GetExceptionInfo(iCorThread, exceptionInfo);
}

HRESULT ManagedDebugger::SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();
    return m_sharedBreakpoints->SetExceptionBreakpoints(exceptionBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::UpdateLineBreakpoint(int id, int linenum, Breakpoint &breakpoint)
{
    LogFuncEntry();

    bool haveProcess = HaveDebugProcess();
    return m_sharedBreakpoints->UpdateLineBreakpoint(haveProcess, id, linenum, breakpoint);
}

#ifdef INTEROP_DEBUGGING

static bool isNativeSource(const std::string &filename)
{
    // Detect native code breakpoint by extension:
    // https://gcc.gnu.org/onlinedocs/gcc-12.2.0/gcc/Overall-Options.html
    static std::unordered_set<std::string> fileExtension{"c", "C", "cc", "cp", "cxx", "cpp", "CPP", "c++",
                                                         "i", "ii", "m", "M", "mi", "mm", "mii",
                                                         "h", "H", "hh", "hp", "hxx", "hpp", "HPP", "h++", "tpp"};

    std::size_t lastDotPos = filename.rfind('.');
    if (lastDotPos != std::string::npos)
    {
        std::string fileExt = filename.substr(lastDotPos + 1);
        auto find = fileExtension.find(fileExt);
        if (find != fileExtension.end())
        {
            return true;
        }
    }

    return false;
}

#endif // INTEROP_DEBUGGING

HRESULT ManagedDebugger::SetLineBreakpoints(const std::string& filename,
                                            const std::vector<LineBreakpoint> &lineBreakpoints,
                                            std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

#ifdef INTEROP_DEBUGGING
    // Note, we don't care about m_interopDebugging here, since breakpoint setup could be start before m_interopDebugging changes with env parsing.
    if (isNativeSource(filename))
        return m_sharedInteropDebugger->SetLineBreakpoints(filename, lineBreakpoints, breakpoints);
#endif // INTEROP_DEBUGGING

    bool haveProcess = HaveDebugProcess();
    return m_sharedBreakpoints->SetLineBreakpoints(haveProcess, filename, lineBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::SetFuncBreakpoints(const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    bool haveProcess = HaveDebugProcess();
    return m_sharedBreakpoints->SetFuncBreakpoints(haveProcess, funcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::BreakpointActivate(int id, bool act)
{
    if (SUCCEEDED(m_sharedBreakpoints->BreakpointActivate(id, act)))
        return S_OK;

#ifdef INTEROP_DEBUGGING
    // Note, we don't care about m_interopDebugging here, since breakpoint setup could be start before m_interopDebugging changes with env parsing.
    return m_sharedInteropDebugger->BreakpointActivate(id, act);
#else
    return E_FAIL;
#endif // INTEROP_DEBUGGING
}

HRESULT ManagedDebugger::AllBreakpointsActivate(bool act)
{
    HRESULT Status1 = m_sharedBreakpoints->AllBreakpointsActivate(act);

#ifdef INTEROP_DEBUGGING
    // Note, we don't care about m_interopDebugging here, since breakpoint setup could be start before m_interopDebugging changes with env parsing.
    HRESULT Status2 = m_sharedInteropDebugger->AllBreakpointsActivate(act);
    return FAILED(Status1) ? Status1 : Status2;
#else
    return Status1;
#endif // INTEROP_DEBUGGING
}

HRESULT ManagedDebuggerBase::GetFrameLocation(ICorDebugFrame *pFrame, ThreadId threadId, FrameLevel level, StackFrame &stackFrame, bool hotReloadAwareCaller)
{
    HRESULT Status;

    stackFrame = StackFrame(threadId, level, "");
    if (FAILED(TypePrinter::GetMethodName(pFrame, stackFrame.methodName)))
        stackFrame.methodName = "Unnamed method in optimized code";

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ULONG32 methodVersion = 1;
    ULONG32 currentVersion = 1;
    if (m_hotReload)
    {
        // In case current (top) code version is 1, executed in this frame method version can't be not 1.
        if (SUCCEEDED(pFunc->GetCurrentVersionNumber(&currentVersion)) && currentVersion != 1)
        {
            ToRelease<ICorDebugCode> pCode;
            IfFailRet(pFunc->GetILCode(&pCode));
            IfFailRet(pCode->GetVersionNumber(&methodVersion));
        }

        if (!hotReloadAwareCaller && methodVersion != currentVersion)
        {
            std::string moduleNamePrefix;
            WCHAR name[mdNameLen];
            ULONG32 name_len = 0;
            if (SUCCEEDED(pModule->GetName(_countof(name), &name_len, name)))
            {
                moduleNamePrefix = to_utf8(name);
                std::size_t i = moduleNamePrefix.find_last_of("/\\");
                if (i != std::string::npos)
                    moduleNamePrefix = moduleNamePrefix.substr(i + 1);
                moduleNamePrefix += "!";
            }

            // [Outdated Code] module.dll!MethodName()
            stackFrame.methodName = "[Outdated Code] " + moduleNamePrefix + stackFrame.methodName;

            return S_OK;
        }
    }

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

    ULONG32 nOffset = 0;
    ToRelease<ICorDebugNativeFrame> pNativeFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &pNativeFrame));
    IfFailRet(pNativeFrame->GetIP(&nOffset));

    IfFailRet(GetModuleId(pModule, stackFrame.moduleId));

    stackFrame.clrAddr.methodToken = methodToken;
    stackFrame.clrAddr.ilOffset = ilOffset;
    stackFrame.clrAddr.nativeOffset = nOffset;
    stackFrame.clrAddr.methodVersion = methodVersion;

    stackFrame.addr = 0; // This method used for managed stop events only, but all implemented protocols don't use `addr` in managed stop events outputs.

    if (stackFrame.clrAddr.ilOffset != 0)
        stackFrame.activeStatementFlags |= StackFrame::ActiveStatementFlags::PartiallyExecuted;
    if (methodVersion == currentVersion)
        stackFrame.activeStatementFlags |= StackFrame::ActiveStatementFlags::MethodUpToDate;
    else
        stackFrame.activeStatementFlags |= StackFrame::ActiveStatementFlags::Stale;

    return S_OK;
}

static std::string GetModuleNameForFrame(ICorDebugFrame *pFrame)
{
    ToRelease<ICorDebugFunction> pFunc;
    if (!pFrame || FAILED(pFrame->GetFunction(&pFunc)))
        return std::string{};

    ToRelease<ICorDebugModule> pModule;
    if (FAILED(pFunc->GetModule(&pModule)))
        return std::string{};

    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;
    if (FAILED(pModule->GetName(_countof(name), &name_len, name)))
        return std::string{};

    return GetBasename(to_utf8(name));
}

HRESULT ManagedDebuggerBase::GetManagedStackTrace(ICorDebugThread *pThread, ThreadId threadId, FrameLevel startFrame, unsigned maxFrames,
                                                  std::vector<StackFrame> &stackFrames, int &totalFrames, bool hotReloadAwareCaller)
{
    LogFuncEntry();

    HRESULT Status;
    int currentFrame = -1;

    auto AddFrameStatementFlag = [&] ()
    {
        if (currentFrame == 0)
            stackFrames.back().activeStatementFlags |= StackFrame::ActiveStatementFlags::LeafFrame;
        else
            stackFrames.back().activeStatementFlags |= StackFrame::ActiveStatementFlags::NonLeafFrame;
    };

#ifdef INTEROP_DEBUGGING
    // In case debug session without interop, we merge "[CoreCLR Native Frame]" and "user's native frame" into "[Native Frames]".
    const std::string FrameCLRNativeText = m_interopDebugging ? "[CoreCLR Native Frame]" : "[Native Frames]";
#else
    // CoreCLR native frame + at least one user's native frame (note, `FrameNative` case should never happen for not interop build)
    static const std::string FrameCLRNativeText = "[Native Frames]";
#endif // INTEROP_DEBUGGING

    IfFailRet(WalkFrames(pThread, [&](
        FrameType frameType,
        std::uintptr_t addr,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative)
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
                stackFrames.back().addr = addr;
                AddFrameStatementFlag();
                break;
            case FrameNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, pNative->procName);
                stackFrames.back().addr = pNative->addr;
                stackFrames.back().unknownFrameAddr = pNative->unknownFrameAddr;
                stackFrames.back().moduleOrLibName = pNative->libName;
                stackFrames.back().source = Source(pNative->fullSourcePath);
                stackFrames.back().line = pNative->lineNum;
                AddFrameStatementFlag();
                break;
            case FrameCLRNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, FrameCLRNativeText);
                stackFrames.back().addr = addr;
                stackFrames.back().unknownFrameAddr = !addr; // Could be 0 here only in case some CoreCLR registers context issue.
                AddFrameStatementFlag();
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
                    stackFrames.back().addr = addr;
                    stackFrames.back().unknownFrameAddr = !addr; // Could be 0 here only in case some CoreCLR registers context issue.
                    AddFrameStatementFlag();
                }
                break;
            case FrameCLRManaged:
                {
                    StackFrame stackFrame;
                    GetFrameLocation(pFrame, threadId, FrameLevel{currentFrame}, stackFrame, hotReloadAwareCaller);
                    stackFrames.push_back(stackFrame);
                    stackFrames.back().addr = addr;
                    stackFrames.back().unknownFrameAddr = !addr; // Could be 0 here only in case some CoreCLR registers context issue.
                    stackFrames.back().moduleOrLibName = GetModuleNameForFrame(pFrame);
                    AddFrameStatementFlag();
                }
                break;
        }

        return S_OK;
    }));

    totalFrames = currentFrame + 1;
    ExceptionInfo exceptionInfo;
    bool analyzeExceptions = true;
    if (!stackFrames.empty())
    {
        analyzeExceptions = analyzeExceptions && (stackFrames.front().line == 0);
    }

#ifdef INTEROP_DEBUGGING
    analyzeExceptions = analyzeExceptions && !m_interopDebugging;
#endif // INTEROP_DEBUGGING

    if (!analyzeExceptions)
        return S_OK;

//  Sometimes Coreclr may return the empty stack frame in exception info
//  for some unknown reason. In that case the 2nd attempt is usually successful.
//  If even the 3rd attempt failed, there is almost no chances to get data successfuly.
    int tries = 3;
    for(int tryCount = 0; tryCount < tries; tryCount++)
    {
        if (SUCCEEDED(GetExceptionInfo (threadId, exceptionInfo)))
        {
            std::stringstream ss(exceptionInfo.details.stackTrace);
            int countOfNewFrames = 0;
            int currentFrame = -1;
            size_t sizeofStackFrame = stackFrames.size();

//          The stackTrace strings from ExceptionInfo usually looks like:
//          at Program.Func2(string[] strvect) in /home/user/work/vscode_test/utils.cs:line 122
//          at Program.Func1<int, char>() in /home/user/work/vscode_test/utils.cs:line 78
//          at Program.Main() in /home/user/work/vscode_test/Program.cs:line 25
//          at Program.Main() in C:\Users/localuser/work/vscode_test\Program.cs:line 25
            while (!ss.eof())
            {
                std::string line;
                std::getline(ss, line, '\n');
                size_t lastcolon = line.find_last_of(':');
                if (lastcolon == std::string::npos)
                    continue;

                size_t beginpath = line.find_first_of('/');
                if (beginpath == std::string::npos)
                {
                    beginpath = line.find_first_of('\\');
                    if (beginpath == std::string::npos)
                    {
                        continue;
                    }
                }

                // append disk name, if exists
                beginpath = line.find_last_of(' ', beginpath);
                if (beginpath == std::string::npos)
                    continue;

                beginpath++;
                if (beginpath >= lastcolon)
                    continue;

                // remove leading spaces and the first word ("at" for the case of English locale)
                size_t beginname = line.find_first_not_of(' ');
                if (beginname == std::string::npos)
                    continue;

                beginname = line.find_first_of(' ', beginname);
                if (beginname == std::string::npos)
                    continue;
                beginname++;

                // the function name ends with the last ')' before the beginning of fullpath
                size_t endname = line.find_last_of(')', beginpath);
                if (endname == std::string::npos)
                    continue;
                endname++;

                if (beginname >= endname)
                    continue;

                // look for the line number after the last colon
                size_t beginlinenum = line.find_first_of("0123456789", lastcolon);
                size_t endlinenum = line.find_first_not_of("0123456789", beginlinenum);
                if (beginlinenum == std::string::npos)
                    continue;

                currentFrame++;
                if (currentFrame < (int)startFrame || (maxFrames != 0 && currentFrame >= (int)startFrame + (int)maxFrames))
                    continue;

                int l{std::stoi(line.substr(beginlinenum, endlinenum))};
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, line.substr(beginname, endname - beginname));
                stackFrames.back().source = Source(line.substr(beginpath, lastcolon - beginpath));
                stackFrames.back().line = stackFrames.back().endLine = l;
                countOfNewFrames++;
            }

            if (countOfNewFrames > 0)
            {
                stackFrames.erase(stackFrames.begin(), stackFrames.begin() + sizeofStackFrame);
                totalFrames = currentFrame + 1;
                break;
            }
        }
    }

    return S_OK;
}

#ifdef INTEROP_DEBUGGING
HRESULT ManagedDebuggerBase::GetNativeStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    LogFuncEntry();

    HRESULT Status;
    int currentFrame = -1;

    Status = m_sharedInteropDebugger->UnwindNativeFrames(int(threadId), true, 0, nullptr, [&](NativeFrame &nativeFrame)
    {
        currentFrame++;

        if (currentFrame < int(startFrame))
            return S_OK;
        if (maxFrames != 0 && currentFrame >= int(startFrame) + int(maxFrames))
            return S_OK;

        stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, nativeFrame.procName);
        stackFrames.back().addr = nativeFrame.addr;
        stackFrames.back().unknownFrameAddr = nativeFrame.unknownFrameAddr;
        stackFrames.back().moduleOrLibName = nativeFrame.libName;
        stackFrames.back().source = Source(nativeFrame.fullSourcePath);
        stackFrames.back().line = nativeFrame.lineNum;

        if (currentFrame == 0)
            stackFrames.back().activeStatementFlags |= StackFrame::ActiveStatementFlags::LeafFrame;
        else
            stackFrames.back().activeStatementFlags |= StackFrame::ActiveStatementFlags::NonLeafFrame;

        return S_OK;
    });

    totalFrames = currentFrame + 1;
    
    return Status;
}
#endif // INTEROP_DEBUGGING

HRESULT ManagedDebugger::GetStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames, bool hotReloadAwareCaller)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    ToRelease<ICorDebugThread> pThread;
    if (SUCCEEDED(Status = m_iCorProcess->GetThread(int(threadId), &pThread)))
        return GetManagedStackTrace(pThread, threadId, startFrame, maxFrames, stackFrames, totalFrames, hotReloadAwareCaller);

#ifdef INTEROP_DEBUGGING
    // E_INVALIDARG for ICorDebugProcess::GetThread() mean thread is not managed (can't found ICorDebugThread object that represents the thread)
    if (m_interopDebugging && Status == E_INVALIDARG)
        return GetNativeStackTrace(threadId, startFrame, maxFrames, stackFrames, totalFrames);
#endif // INTEROP_DEBUGGING

    return Status;
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

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedVariables->GetVariables(m_iCorProcess, variablesReference, filter, start, count, variables);
}

HRESULT ManagedDebugger::GetScopes(FrameId frameId, std::vector<Scope> &scopes)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedVariables->GetScopes(m_iCorProcess, frameId, scopes);
}

HRESULT ManagedDebugger::Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedVariables->Evaluate(m_iCorProcess, frameId, expression, variable, output);
}

void ManagedDebugger::CancelEvalRunning()
{
    LogFuncEntry();

    m_sharedEvalWaiter->CancelEvalRunning();
}

HRESULT ManagedDebugger::SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedVariables->SetVariable(m_iCorProcess, name, value, ref, output);
}

HRESULT ManagedDebugger::SetExpression(FrameId frameId, const std::string &expression, int evalFlags, const std::string &value, std::string &output)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    return m_sharedVariables->SetExpression(m_iCorProcess, frameId, expression, evalFlags, value, output);
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

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    if (FAILED(CheckDebugProcess()))
        return;

    StackFrame frame{thread, framelevel, ""};
    std::vector<Scope> scopes;
    std::vector<Variable> variables;
    HRESULT status = m_sharedVariables->GetScopes(m_iCorProcess, frame.id, scopes);
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

    status = m_sharedVariables->GetVariables(m_iCorProcess, scopes[0].variablesReference, VariablesNamed, 0, 0, variables);
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


void ManagedDebuggerBase::InputCallback(IORedirectHelper::StreamType type, span<char> text)
{
    pProtocol->EmitOutputEvent(type == IOSystem::Stderr ? OutputStdErr : OutputStdOut, {text.begin(), text.size()});
}


void ManagedDebugger::EnumerateBreakpoints(std::function<bool (const BreakpointInfo&)>&& callback)
{
    LogFuncEntry();
    return m_sharedBreakpoints->EnumerateBreakpoints(std::move(callback));
}

static HRESULT GetModuleOfCurrentThreadCode(ICorDebugProcess *pProcess, int lastStoppedThreadId, ICorDebugModule **ppModule)
{
    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(pProcess->GetThread(lastStoppedThreadId, &pThread));

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));
    return pFunc->GetModule(ppModule);
}

HRESULT ManagedDebugger::GetSourceFile(const std::string &sourcePath, char** fileBuf, int* fileLen)
{
    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);
    HRESULT Status;
    IfFailRet(CheckDebugProcess());

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(GetModuleOfCurrentThreadCode(m_iCorProcess, int(GetLastStoppedThreadId()), &pModule));
    return m_sharedModules->GetSource(pModule, sourcePath, fileBuf, fileLen);
}

void ManagedDebugger::FreeUnmanaged(PVOID mem)
{
    Interop::CoTaskMemFree(mem);
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
    m_sharedBreakpoints->SetJustMyCode(enable);
}

void ManagedDebugger::SetStepFiltering(bool enable)
{
    m_stepFiltering = enable;
    m_uniqueSteppers->SetStepFiltering(enable);
}

HRESULT ManagedDebugger::SetHotReload(bool enable)
{
    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);

    if (m_iCorProcess && m_startMethod == StartAttach)
        return CORDBG_E_CANNOT_BE_ON_ATTACH;

    m_hotReload = enable;

    return S_OK;
}

#ifdef INTEROP_DEBUGGING
void ManagedDebugger::SetInteropDebugging(bool enable)
{
    m_interopDebugging = enable;
}
#endif

static HRESULT ApplyMetadataAndILDeltas(Modules *pModules, const std::string &dllFileName, const std::string &deltaMD, const std::string &deltaIL)
{
    HRESULT Status;

    std::ifstream deltaILFileStream(deltaIL, std::ios::in | std::ios::binary | std::ios::ate);
    std::ifstream deltaMDFileStream(deltaMD, std::ios::in | std::ios::binary | std::ios::ate);

    if (!deltaILFileStream.is_open() || !deltaMDFileStream.is_open())
        return COR_E_FILENOTFOUND;

    auto deltaILSize = deltaILFileStream.tellg();
    if (deltaILSize < 0)
        return E_FAIL;
    std::unique_ptr<BYTE[]> deltaILMemBlock(new BYTE[(size_t)deltaILSize]);
    deltaILFileStream.seekg(0, std::ios::beg);
    deltaILFileStream.read((char*)deltaILMemBlock.get(), deltaILSize);

    auto deltaMDSize = deltaMDFileStream.tellg();
    if (deltaMDSize < 0)
        return E_FAIL;
    std::unique_ptr<BYTE[]> deltaMDMemBlock(new BYTE[(size_t)deltaMDSize]);
    deltaMDFileStream.seekg(0, std::ios::beg);
    deltaMDFileStream.read((char*)deltaMDMemBlock.get(), deltaMDSize);

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pModules->GetModuleWithName(dllFileName, &pModule, true));
    ToRelease<ICorDebugModule2> pModule2;
    IfFailRet(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2));
    IfFailRet(pModule2->ApplyChanges((ULONG)deltaMDSize, deltaMDMemBlock.get(), (ULONG)deltaILSize, deltaILMemBlock.get()));

    return S_OK;
}

HRESULT ManagedDebuggerBase::ApplyPdbDeltaAndLineUpdates(const std::string &dllFileName, const std::string &deltaPDB, const std::string &lineUpdates,
                                                         std::string &updatedDLL, std::unordered_set<mdTypeDef> &updatedTypeTokens)
{
    HRESULT Status;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(m_sharedModules->GetModuleWithName(dllFileName, &pModule, true));

    std::unordered_set<mdMethodDef> pdbMethodTokens;
    IfFailRet(m_sharedModules->ApplyPdbDeltaAndLineUpdates(pModule, m_justMyCode, deltaPDB, lineUpdates, pdbMethodTokens));

    updatedDLL = GetModuleFileName(pModule);
    for (const auto &methodToken : pdbMethodTokens)
    {
        mdTypeDef typeDef;
        ToRelease<ICorDebugFunction> iCorFunction;
        ToRelease<ICorDebugClass> iCorClass;
        if (SUCCEEDED(pModule->GetFunctionFromToken(methodToken, &iCorFunction)) &&
            SUCCEEDED(iCorFunction->GetClass(&iCorClass)) &&
            SUCCEEDED(iCorClass->GetToken(&typeDef)))
            updatedTypeTokens.insert(typeDef);
    }

    // Since we could have new code lines and new methods added, check all breakpoints again.
    std::vector<BreakpointEvent> events;
    m_sharedBreakpoints->UpdateBreakpointsOnHotReload(pModule, pdbMethodTokens, events);
    for (const BreakpointEvent &event : events)
        pProtocol->EmitBreakpointEvent(event);

    return S_OK;
}

HRESULT ManagedDebuggerBase::FindEvalCapableThread(ToRelease<ICorDebugThread> &pThread)
{
    ThreadId lastStoppedId = GetLastStoppedThreadId();
    std::vector<ThreadId> threadIds;
    m_sharedThreads->GetThreadIds(threadIds);
    for (size_t i = 0; i < threadIds.size(); ++i)
    {
        if (threadIds[i] == lastStoppedId)
        {
            std::swap(threadIds[0], threadIds[i]);
            break;
        }
    }

    for (auto &threadId : threadIds)
    {
        ToRelease<ICorDebugValue> iCorValue;
        if (SUCCEEDED(m_iCorProcess->GetThread(int(threadId), &pThread)) &&
            SUCCEEDED(m_sharedEvalHelpers->CreateString(pThread, "test_string", &iCorValue)))
        {
            return S_OK;
        }
        pThread.Free();
    }

    return E_FAIL;
}

HRESULT ManagedDebugger::HotReloadApplyDeltas(const std::string &dllFileName, const std::string &deltaMD, const std::string &deltaIL,
                                              const std::string &deltaPDB, const std::string &lineUpdates)
{
    LogFuncEntry();

    std::lock_guard<Utility::RWLock::Reader> guardProcessRWLock(m_debugProcessRWLock.reader);

    if (!m_iCorProcess)
        return E_FAIL;

    // Deltas can be applied only on stopped debuggee process. For Hot Reload scenario we temporary stop it and continue after deltas applied.
    HRESULT Status;
    IfFailRet(m_sharedCallbacksQueue->Stop(m_iCorProcess));
    bool continueProcess = (Status == S_OK); // Was stopped by m_sharedCallbacksQueue->Stop() call.

    IfFailRet(ApplyMetadataAndILDeltas(m_sharedModules.get(), dllFileName, deltaMD, deltaIL));
    std::string updatedDLL;
    std::unordered_set<mdTypeDef> updatedTypeTokens;
    IfFailRet(ApplyPdbDeltaAndLineUpdates(dllFileName, deltaPDB, lineUpdates, updatedDLL, updatedTypeTokens));

    ToRelease<ICorDebugThread> pThread;
    if (SUCCEEDED(FindEvalCapableThread(pThread)))
        IfFailRet(HotReloadHelpers::UpdateApplication(pThread, m_sharedModules.get(), m_sharedEvaluator.get(), m_sharedEvalHelpers.get(), updatedDLL, updatedTypeTokens));
    else
        IfFailRet(m_sharedBreakpoints->SetHotReloadBreakpoint(updatedDLL, updatedTypeTokens));

    if (continueProcess)
        IfFailRet(m_sharedCallbacksQueue->Continue(m_iCorProcess));

    return S_OK;
}

} // namespace netcoredbg
