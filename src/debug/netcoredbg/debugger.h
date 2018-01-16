// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "protocol.h"
#include <unordered_map>

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

    bool m_justMyCode;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;

    enum ValueKind
    {
        ValueIsScope,
        ValueIsClass,
        ValueIsVariable
    };

    struct VariableReference
    {
        uint32_t variablesReference; // key
        int namedVariables;
        int indexedVariables;

        std::string evaluateName;

        ValueKind valueKind;
        ToRelease<ICorDebugValue> value;
        uint64_t frameId;

        VariableReference(const Variable &variable, uint64_t frameId, ToRelease<ICorDebugValue> value, ValueKind valueKind) :
            variablesReference(variable.variablesReference),
            namedVariables(variable.namedVariables),
            indexedVariables(variable.indexedVariables),
            evaluateName(variable.evaluateName),
            valueKind(valueKind),
            value(std::move(value)),
            frameId(frameId)
        {}

        VariableReference(uint32_t variablesReference, uint64_t frameId, int namedVariables) :
            variablesReference(variablesReference),
            namedVariables(namedVariables),
            indexedVariables(0),
            valueKind(ValueIsScope),
            value(nullptr),
            frameId(frameId)
        {}

        bool IsScope() const { return valueKind == ValueIsScope; }

        VariableReference(VariableReference &&that) = default;
    private:
        VariableReference(const VariableReference &that) = delete;
    };
    std::unordered_map<uint32_t, VariableReference> m_variables;
    uint32_t m_nextVariableReference;

    void AddVariableReference(Variable &variable, uint64_t frameId, ICorDebugValue *value, ValueKind valueKind);

    HRESULT CheckNoProcess();

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    void Cleanup();

    HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);

    HRESULT GetStackVariables(uint64_t frameId, ICorDebugThread *pThread, ICorDebugFrame *pFrame, int start, int count, std::vector<Variable> &variables);
    HRESULT GetChildren(VariableReference &ref, ICorDebugThread *pThread, ICorDebugFrame *pFrame, int start, int count, std::vector<Variable> &variables);

    HRESULT EvalExpr(ICorDebugThread *pThread,
                     ICorDebugFrame *pFrame,
                     const std::string &expression,
                     ICorDebugValue **ppResult);
    HRESULT FollowNested(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         const std::string &methodClass,
                         const std::vector<std::string> &parts,
                         ICorDebugValue **ppResult);
    HRESULT FollowFields(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         ICorDebugValue *pValue,
                         ValueKind valueKind,
                         const std::vector<std::string> &parts,
                         int nextPart,
                         ICorDebugValue **ppResult);
    HRESULT GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                       ICorDebugILFrame *pILFrame,
                                       ICorDebugValue *pInputValue,
                                       ValueKind valueKind,
                                       const std::string &name,
                                       ICorDebugValue **ppResultValue);

    ToRelease<ICorDebugFunction> m_pRunClassConstructor;
    ToRelease<ICorDebugFunction> m_pGetTypeHandle;
    HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue);

public:
    Debugger();
    ~Debugger();

    bool IsJustMyCode() { return m_justMyCode; }
    void SetJustMyCode(bool enable) { m_justMyCode = enable; }

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule);

    void SetProtocol(Protocol *protocol) { m_protocol = protocol; }

    HRESULT RunProcess(std::string fileExec, std::vector<std::string> execArgs);

    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

    HRESULT Continue();
    HRESULT Pause();
    HRESULT GetThreads(std::vector<Thread> &threads);
    HRESULT SetBreakpoint(std::string filename, int linenum, Breakpoint &breakpoint);
    HRESULT GetStackTrace(int threadId, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames);
    HRESULT StepCommand(int threadId, StepType stepType);
    HRESULT GetScopes(uint64_t frameId, std::vector<Scope> &scopes);
    HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables);
    int GetNamedVariables(uint32_t variablesReference);
    HRESULT Evaluate(uint64_t frameId, const std::string &expression, Variable &variable);
};

class Protocol
{
public:
    virtual void EmitStoppedEvent(StoppedEvent event) = 0;
    virtual void EmitExitedEvent(ExitedEvent event) = 0;
    virtual void EmitThreadEvent(ThreadEvent event) = 0;
    virtual void EmitOutputEvent(OutputEvent event) = 0;
    virtual void EmitBreakpointEvent(BreakpointEvent event) = 0;
    virtual void Cleanup() = 0;
    virtual void CommandLoop() = 0;
    virtual ~Protocol() {}
};

class MIProtocol : public Protocol
{
    static std::mutex m_outMutex;
    bool m_exit;
    Debugger *m_debugger;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    unsigned int m_varCounter;
    std::unordered_map<std::string, Variable> m_vars;
public:
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; m_debugger->SetProtocol(this); }
    static std::string EscapeMIValue(const std::string &str);

    MIProtocol() : m_exit(false), m_varCounter(0) {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;

    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));

private:
    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
    HRESULT PrintFrames(int threadId, std::string &output, int lowFrame, int highFrame);
    HRESULT PrintVariables(const std::vector<Variable> &variables, std::string &output);
    HRESULT CreateVar(int threadId, int level, const std::string &varobjName, const std::string &expression, std::string &output);
    HRESULT DeleteVar(const std::string &varobjName);
    void PrintChildren(std::vector<Variable> &children, int threadId, int print_values, bool has_more, std::string &output);
    void PrintNewVar(std::string varobjName, Variable &v, int threadId, int print_values, std::string &output);
    HRESULT ListChildren(int threadId, int level, int childStart, int childEnd, const std::string &varName, int print_values, std::string &output);
};

HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);
int GetLastStoppedThreadId();
void WaitProcessExited();

HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
