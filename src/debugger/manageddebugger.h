// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <vector>
#include <map>
#include <set>
#include "interfaces/idebugger.h"
#include "debugger/dbgshim.h"
#include "string_view.h"
#include "span.h"
#include "ioredirect.h"
#include "torelease.h"

namespace netcoredbg
{

using Utility::string_view;
template <typename T> using span = Utility::span<T>;

ThreadId getThreadId(ICorDebugThread *pThread);

class Evaluator;
class Variables;
class ManagedCallback;
class Breakpoints;
class Modules;

class ManagedDebugger : public IDebugger
{
private:
    friend class ManagedCallback;
    enum ProcessAttachedState
    {
        ProcessAttached,
        ProcessUnattached
    };
    std::mutex m_processAttachedMutex;
    std::condition_variable m_processAttachedCV;
    ProcessAttachedState m_processAttachedState;

    void NotifyProcessCreated();
    void NotifyProcessExited();
    void WaitProcessExited();
    HRESULT CheckNoProcess();

    struct FullyQualifiedIlOffset_t
    {
        CORDB_ADDRESS modAddress = 0;
        mdMethodDef methodToken = 0;
        ULONG32 ilOffset = 0;

        void Reset()
        {
            modAddress = 0;
            methodToken = 0;
            ilOffset = 0;
        }
    };

    std::mutex m_lastStoppedMutex;
    ThreadId m_lastStoppedThreadId;
    FullyQualifiedIlOffset_t m_lastStoppedIlOffset;

    std::mutex m_lastUnhandledExceptionThreadIdsMutex;
    std::set<ThreadId> m_lastUnhandledExceptionThreadIds;

    void SetLastStoppedThread(ICorDebugThread *pThread);
    void SetLastStoppedThreadId(ThreadId threadId);
    HRESULT GetFullyQualifiedIlOffset(const ThreadId &pThread, FullyQualifiedIlOffset_t &fullyQualifiedIlOffset);

    std::mutex m_stopCounterMutex;
    int m_stopCounter;

    enum StartMethod
    {
        StartNone,
        StartLaunch,
        StartAttach
        //StartAttachForSuspendedLaunch
    } m_startMethod;
    std::string m_execPath;
    std::vector<std::string> m_execArgs;
    std::string m_cwd;
    std::map<std::string, std::string> m_env;
    bool m_stopAtEntry;
    bool m_isConfigurationDone;

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    std::unique_ptr<Breakpoints> m_uniqueBreakpoints;
    std::shared_ptr<Variables> m_sharedVariables;
    std::unique_ptr<ManagedCallback> m_managedCallback;
    ToRelease<ICorDebug> m_iCorDebug;
    ToRelease<ICorDebugProcess> m_iCorProcess;

    std::mutex m_stepMutex;
    int m_enabledSimpleStepId;

    bool m_justMyCode;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;
    dbgshim_t m_dbgshim;

    IORedirectHelper m_ioredirect;

    void InputCallback(IORedirectHelper::StreamType, span<char> text);

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, DWORD pid);

    void Cleanup();

    HRESULT DisableAllSteppers();
    HRESULT DisableAllSimpleSteppers();
    HRESULT DisableAllBreakpointsAndSteppers();

    HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);
    HRESULT SetupSimpleStep(ICorDebugThread *pThread, StepType stepType);

    HRESULT GetStackTrace(ICorDebugThread *pThread, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames);
    HRESULT GetFrameLocation(ICorDebugFrame *pFrame, ThreadId threadId, FrameLevel level, StackFrame &stackFrame);

    HRESULT RunProcess(const std::string& fileExec, const std::vector<std::string>& execArgs);
    HRESULT AttachToProcess(DWORD pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    HRESULT RunIfReady();

    HRESULT SetEnableCustomNotification(BOOL fEnable);

public:
    ManagedDebugger();
    ~ManagedDebugger() override;

    bool IsJustMyCode() const override { return m_justMyCode; }
    void SetJustMyCode(bool enable) override { m_justMyCode = enable; }

    HRESULT Initialize() override;
    HRESULT Attach(int pid) override;
    HRESULT Launch(const std::string &fileExec, const std::vector<std::string> &execArgs, const std::map<std::string, std::string> &env,
        const std::string &cwd, bool stopAtEntry = false) override;
    HRESULT ConfigurationDone() override;

    HRESULT Disconnect(DisconnectAction action = DisconnectDefault) override;

    ThreadId GetLastStoppedThreadId() override;
    void InvalidateLastStoppedThreadId();
    HRESULT Continue(ThreadId threadId) override;
    HRESULT Pause() override;
    HRESULT GetThreads(std::vector<Thread> &threads) override;
    HRESULT SetBreakpoints(const std::string& filename, const std::vector<SourceBreakpoint> &srcBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT SetFunctionBreakpoints(const std::vector<FunctionBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT BreakpointActivate(int id, bool act) override;
    HRESULT AllBreakpointsActivate(bool act) override;
    HRESULT GetStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames) override;
    HRESULT StepCommand(ThreadId threadId, StepType stepType) override;
    HRESULT GetScopes(FrameId frameId, std::vector<Scope> &scopes) override;
    HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) override;
    int GetNamedVariables(uint32_t variablesReference) override;
    HRESULT Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output) override;
    HRESULT SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output) override;
    HRESULT SetVariableByExpression(FrameId frameId, const Variable &variable, const std::string &value, std::string &output) override;
    HRESULT GetExceptionInfoResponse(ThreadId threadId, ExceptionInfoResponse &exceptionResponse) override;
    HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t &output) override;
    HRESULT DeleteExceptionBreakpoint(const uint32_t id) override;

    void FindFileNames(string_view pattern, unsigned limit, SearchCallback) override;
    void FindFunctions(string_view pattern, unsigned limit, SearchCallback) override;
    void FindVariables(ThreadId, FrameLevel, string_view pattern, unsigned limit, SearchCallback) override;

    void EnumerateBreakpoints(std::function<bool (const IDebugger::BreakpointInfo&)>&& callback) override;

    // Functions which converts FrameId to ThreadId and FrameLevel and vice versa.
    FrameId getFrameId(ThreadId, FrameLevel);
    ThreadId threadByFrameId(FrameId) const;
    FrameLevel frameByFrameId(FrameId) const;

    // pass some data to debugee stdin
    IDebugger::AsyncResult ProcessStdin(InStream &) override;

private:
    bool MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category);

    enum class asyncStepStatus
    {
        yield_offset_breakpoint,
        resume_offset_breakpoint
    };

    struct asyncBreakpoint_t
    {
        ToRelease<ICorDebugBreakpoint> iCorBreakpoint;
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 ilOffset;

        asyncBreakpoint_t() :
            iCorBreakpoint(nullptr),
            modAddress(0),
            methodToken(0),
            ilOffset(0)
        {}

        ~asyncBreakpoint_t()
        {
            if (iCorBreakpoint)
                iCorBreakpoint->Activate(FALSE);
        }
    };

    struct asyncStep_t
    {
        ThreadId m_threadId;
        IDebugger::StepType m_initialStepType;
        uint32_t m_resume_offset;
        asyncStepStatus m_stepStatus;
        std::unique_ptr<asyncBreakpoint_t> m_Breakpoint;
        ToRelease<ICorDebugReferenceValue> pValueAsyncIdRef;

        asyncStep_t() :
            m_threadId(ThreadId::Invalid),
            m_initialStepType(IDebugger::StepType::STEP_OVER),
            m_resume_offset(0),
            m_stepStatus(asyncStepStatus::yield_offset_breakpoint),
            m_Breakpoint(nullptr),
            pValueAsyncIdRef(nullptr)
        {}
    };

    std::mutex m_asyncStepMutex;
    // Pointer to object, that provide all active async step related data. Object will be created only in case of active async method stepping.
    std::unique_ptr<asyncStep_t> m_asyncStep;
    // System.Threading.Tasks.Task.NotifyDebuggerOfWaitCompletion() method function breakpoint data, will be configured at async method step-out setup.
    std::unique_ptr<asyncBreakpoint_t> m_asyncStepNotifyDebuggerOfWaitCompletion;

    bool HitAsyncStepBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);
    HRESULT SetBreakpointIntoNotifyDebuggerOfWaitCompletion();
};

} // namespace netcoredbg
