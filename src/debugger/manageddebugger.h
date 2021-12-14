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
#include "utils/string_view.h"
#include "utils/span.h"
#include "utils/ioredirect.h"
#include "utils/torelease.h"
#include "utils/rwlock.h"

namespace netcoredbg
{

using Utility::string_view;
template <typename T> using span = Utility::span<T>;

class Threads;
class Steppers;
class Evaluator;
class EvalWaiter;
class EvalHelpers;
class EvalStackMachine;
class Variables;
class ManagedCallback;
class Breakpoints;
class Modules;

enum class ProcessAttachedState
{
    Attached,
    Unattached
};

class ManagedDebugger : public IDebugger
{
private:
    friend class ManagedCallback;

    std::mutex m_processAttachedMutex; // Note, in case m_debugProcessRWLock+m_processAttachedMutex, m_debugProcessRWLock must be locked first.
    std::condition_variable m_processAttachedCV;
    ProcessAttachedState m_processAttachedState;

    void NotifyProcessCreated();
    void NotifyProcessExited();
    HRESULT CheckNoProcess();

    std::mutex m_lastStoppedMutex;
    ThreadId m_lastStoppedThreadId;

    void SetLastStoppedThread(ICorDebugThread *pThread);
    void SetLastStoppedThreadId(ThreadId threadId);

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
    bool m_isConfigurationDone;

    std::shared_ptr<Threads> m_sharedThreads;
    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalWaiter> m_sharedEvalWaiter;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    std::shared_ptr<EvalStackMachine> m_sharedEvalStackMachine;
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    std::shared_ptr<Variables> m_sharedVariables;
    std::unique_ptr<Steppers> m_uniqueSteppers;
    std::unique_ptr<Breakpoints> m_uniqueBreakpoints;
    std::unique_ptr<ManagedCallback> m_managedCallback;

    Utility::RWLock m_debugProcessRWLock;
    ToRelease<ICorDebug> m_iCorDebug;
    ToRelease<ICorDebugProcess> m_iCorProcess;

    bool m_justMyCode;
    bool m_stepFiltering;

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

    void DisableAllBreakpointsAndSteppers();
    HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);

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
    void SetJustMyCode(bool enable) override;
    bool IsStepFiltering() const override { return m_stepFiltering; }
    void SetStepFiltering(bool enable) override;

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
    HRESULT SetLineBreakpoints(const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT SetFuncBreakpoints(const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT BreakpointActivate(int id, bool act) override;
    void EnumerateBreakpoints(std::function<bool (const IDebugger::BreakpointInfo&)>&& callback) override;
    HRESULT AllBreakpointsActivate(bool act) override;
    HRESULT GetStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames) override;
    HRESULT StepCommand(ThreadId threadId, StepType stepType) override;
    HRESULT GetScopes(FrameId frameId, std::vector<Scope> &scopes) override;
    HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) override;
    int GetNamedVariables(uint32_t variablesReference) override;
    HRESULT Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output) override;
    HRESULT SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output) override;
    HRESULT SetExpression(FrameId frameId, const std::string &expression, int evalFlags, const std::string &value, std::string &output) override;
    HRESULT GetExceptionInfo(ThreadId threadId, ExceptionInfo &exceptionInfo) override;
    HRESULT GetSourceFile(const std::string &sourcePath, char** fileBuf, int* fileLen) override;
    void FreeUnmanaged(PVOID mem) override;

    void FindFileNames(string_view pattern, unsigned limit, SearchCallback) override;
    void FindFunctions(string_view pattern, unsigned limit, SearchCallback) override;
    void FindVariables(ThreadId, FrameLevel, string_view pattern, unsigned limit, SearchCallback) override;

    // pass some data to debugee stdin
    IDebugger::AsyncResult ProcessStdin(InStream &) override;
};

} // namespace netcoredbg
