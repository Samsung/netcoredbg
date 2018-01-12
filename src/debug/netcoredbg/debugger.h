// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "protocol.h"


class ManagedCallback;
class Protocol;

class Debugger
{
public:
    enum StepType {
        STEP_IN = 0,
        STEP_OVER,
        STEP_OUT
    };

private:
    friend class ManagedCallback;
    Protocol *m_protocol;
    ManagedCallback *m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;

    static bool m_justMyCode;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;

    HRESULT CheckNoProcess();

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    void Cleanup();

    static HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);

public:
    static bool IsJustMyCode() { return m_justMyCode; }
    static void SetJustMyCode(bool enable) { m_justMyCode = enable; }

    Debugger() :
        m_managedCallback(nullptr),
        m_pDebug(nullptr),
        m_pProcess(nullptr),
        m_startupReady(false),
        m_startupResult(S_OK),
        m_unregisterToken(nullptr),
        m_processId(0) {}

    ~Debugger();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule);

    void SetProtocol(Protocol *protocol) { m_protocol = protocol; }
    void SetManagedCallback(ManagedCallback *managedCallback);

    HRESULT RunProcess(std::string fileExec, std::vector<std::string> execArgs);

    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    ICorDebugProcess *GetProcess() { return m_pProcess; }

    HRESULT StepCommand(int threadId, StepType stepType);
};

class Protocol
{
public:
    virtual void EmitStoppedEvent(StoppedEvent event) = 0;
    virtual void EmitExitedEvent(ExitedEvent event) = 0;
    virtual void EmitThreadEvent(ThreadEvent event) = 0;
    virtual void EmitOutputEvent(OutputEvent event) = 0;
    virtual void EmitBreakpointEvent(BreakpointEvent event) = 0;
    virtual void CommandLoop() = 0;
};

class MIProtocol : public Protocol
{
    static std::mutex m_outMutex;
    bool m_exit;
    Debugger *m_debugger;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;
public:
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; m_debugger->SetProtocol(this); }
    static std::string EscapeMIValue(const std::string &str);

    MIProtocol() : m_exit(false) {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void CommandLoop() override;

    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));

private:
    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
};

HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);
int GetLastStoppedThreadId();
void WaitProcessExited();

HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
