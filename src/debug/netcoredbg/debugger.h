// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "protocol.h"
#include "modules.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include <future>

class ManagedCallback;
class Protocol;
class Debugger;

enum ValueKind
{
    ValueIsScope,
    ValueIsClass,
    ValueIsVariable
};

class Evaluator
{
public:

    typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
    typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

    Modules &m_modules;
private:

    ToRelease<ICorDebugFunction> m_pRunClassConstructor;
    ToRelease<ICorDebugFunction> m_pGetTypeHandle;

    std::mutex m_evalMutex;
    std::unordered_map< DWORD, std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > > m_evalResults;

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

    HRESULT WaitEvalResult(ICorDebugThread *pThread,
                           ICorDebugEval *pEval,
                           ICorDebugValue **ppEvalResult);

    HRESULT EvalObjectNoConstructor(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugValue **ppEvalResult);

    std::future< std::unique_ptr<ToRelease<ICorDebugValue>> > RunEval(
        ICorDebugThread *pThread,
        ICorDebugEval *pEval);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        ICorDebugType *pTypeCast,
        WalkMembersCallback cb);

    HRESULT HandleSpecialLocalVar(
        const std::string &localName,
        ICorDebugValue *pLocalValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT HandleSpecialThisParam(
        ICorDebugValue *pThisValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT GetLiteralValue(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugModule *pModule,
        PCCOR_SIGNATURE pSignatureBlob,
        ULONG sigBlobLength,
        UVCP_CONSTANT pRawValue,
        ULONG rawValueLength,
        ICorDebugValue **ppLiteralValue);

    HRESULT FindType(
        const std::vector<std::string> &parts,
        int &nextPart,
        ICorDebugThread *pThread,
        ICorDebugModule *pModule,
        ICorDebugType **ppType,
        ICorDebugModule **ppModule = nullptr);

    HRESULT ResolveParameters(
        const std::vector<std::string> &params,
        ICorDebugThread *pThread,
        std::vector< ToRelease<ICorDebugType> > &types);

public:

    Evaluator(Modules &modules) : m_modules(modules) {}

    HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue);

    HRESULT EvalFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType *pType, // may be nullptr
        ICorDebugValue *pArgValue, // may be nullptr
        ICorDebugValue **ppEvalResult);

    HRESULT EvalExpr(ICorDebugThread *pThread,
                     ICorDebugFrame *pFrame,
                     const std::string &expression,
                     ICorDebugValue **ppResult);

    bool IsEvalRunning();

    // Should be called by ICorDebugManagedCallback
    void NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval);

    HRESULT ObjectToString(
        ICorDebugThread *pThread,
        ICorDebugValue *pValue,
        std::function<void(const std::string&)> cb
    );

    HRESULT GetType(
        const std::string &typeName,
        ICorDebugThread *pThread,
        ICorDebugType **ppType);

    HRESULT WalkMembers(
        ICorDebugValue *pValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb);

    void Cleanup();
};

class Breakpoints
{
    struct ManagedBreakpoint {
        uint32_t id;
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 ilOffset;
        std::string fullname;
        int linenum;
        ToRelease<ICorDebugBreakpoint> breakpoint;
        bool enabled;
        ULONG32 times;

        bool IsResolved() const { return modAddress != 0; }

        ManagedBreakpoint();
        ~ManagedBreakpoint();

        void ToBreakpoint(Breakpoint &breakpoint);

        ManagedBreakpoint(ManagedBreakpoint &&that) = default;
        ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
    };

    Modules &m_modules;
    uint32_t m_nextBreakpointId;
    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, std::unordered_map<int, ManagedBreakpoint> > m_breakpoints;

    HRESULT ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp);
    HRESULT ResolveBreakpoint(ManagedBreakpoint &bp);

public:
    Breakpoints(Modules &modules) : m_modules(modules), m_nextBreakpointId(1) {}

    HRESULT HitBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint);
    void DeleteAllBreakpoints();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

    void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint);

    HRESULT SetBreakpoints(
        ICorDebugProcess *pProcess,
        std::string filename,
        const std::vector<int> &lines,
        std::vector<Breakpoint> &breakpoints);
};

class Variables
{
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
        VariableReference(const VariableReference &that) = delete;
    };

    Evaluator &m_evaluator;

    std::unordered_map<uint32_t, VariableReference> m_variables;
    uint32_t m_nextVariableReference;

    void AddVariableReference(Variable &variable, uint64_t frameId, ICorDebugValue *value, ValueKind valueKind);

    HRESULT GetStackVariables(
        uint64_t frameId,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT GetChildren(
        VariableReference &ref,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        int start,
        int count,
        std::vector<Variable> &variables);

    struct Member;

    static void FixupInheritedFieldNames(std::vector<Member> &members);

    HRESULT FetchFieldsAndProperties(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        std::vector<Member> &members,
        bool fetchOnlyStatic,
        bool &hasStaticMembers,
        int childStart,
        int childEnd);

    HRESULT GetNumChild(
        ICorDebugValue *pValue,
        unsigned int &numchild,
        bool static_members = false);

public:

    Variables(Evaluator &evaluator) : m_evaluator(evaluator), m_nextVariableReference(1) {}

    int GetNamedVariables(uint32_t variablesReference);

    HRESULT Variables::GetVariables(
        ICorDebugProcess *pProcess,
        uint32_t variablesReference,
        VariablesFilter filter,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT GetScopes(ICorDebugProcess *pProcess, uint64_t frameId, std::vector<Scope> &scopes);

    HRESULT Evaluate(ICorDebugProcess *pProcess, uint64_t frameId, const std::string &expression, Variable &variable);

    void Clear() { m_variables.clear(); m_nextVariableReference = 1; }
};

class Debugger
{
public:
    enum StepType {
        STEP_IN = 0,
        STEP_OVER,
        STEP_OUT
    };

    enum DisconnectAction
    {
        DisconnectDefault, // Attach -> Detach, Launch -> Terminate
        DisconnectTerminate,
        DisconnectDetach
    };

    virtual ~Debugger() {}

    virtual bool IsJustMyCode() = 0;
    virtual void SetJustMyCode(bool enable) = 0;

    virtual HRESULT Initialize() = 0;
    virtual HRESULT Attach(int pid) = 0;
    virtual HRESULT Launch(std::string fileExec, std::vector<std::string> execArgs, bool stopAtEntry = false) = 0;
    virtual HRESULT ConfigurationDone() = 0;

    virtual HRESULT Disconnect(DisconnectAction action = DisconnectDefault) = 0;

    virtual int GetLastStoppedThreadId() = 0;

    virtual HRESULT Continue() = 0;
    virtual HRESULT Pause() = 0;
    virtual HRESULT GetThreads(std::vector<Thread> &threads) = 0;
    virtual HRESULT SetBreakpoints(std::string filename, const std::vector<int> &lines, std::vector<Breakpoint> &breakpoints) = 0;
    virtual void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint) = 0;
    virtual HRESULT GetStackTrace(int threadId, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames) = 0;
    virtual HRESULT StepCommand(int threadId, StepType stepType) = 0;
    virtual HRESULT GetScopes(uint64_t frameId, std::vector<Scope> &scopes) = 0;
    virtual HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) = 0;
    virtual int GetNamedVariables(uint32_t variablesReference) = 0;
    virtual HRESULT Evaluate(uint64_t frameId, const std::string &expression, Variable &variable) = 0;
};

class ManagedDebugger : public Debugger
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

    std::mutex m_lastStoppedThreadIdMutex;
    int m_lastStoppedThreadId;

    void SetLastStoppedThread(ICorDebugThread *pThread);

    enum StartMethod
    {
        StartNone,
        StartLaunch,
        StartAttach
        //StartAttachForSuspendedLaunch
    } m_startMethod;
    std::string m_execPath;
    std::vector<std::string> m_execArgs;
    bool m_stopAtEntry;

    Modules m_modules;
    Evaluator m_evaluator;
    Breakpoints m_breakpoints;
    Variables m_variables;
    Protocol *m_protocol;
    ToRelease<ManagedCallback> m_managedCallback;
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

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, int pid);

    void Cleanup();

    static HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);

    HRESULT SetupStep(ICorDebugThread *pThread, StepType stepType);

    HRESULT GetStackTrace(ICorDebugThread *pThread, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames);
    HRESULT GetFrameLocation(ICorDebugFrame *pFrame, int threadId, uint32_t level, StackFrame &stackFrame);

    HRESULT RunProcess(std::string fileExec, std::vector<std::string> execArgs);
    HRESULT AttachToProcess(int pid);
    HRESULT DetachFromProcess();
    HRESULT TerminateProcess();

public:
    ManagedDebugger();
    ~ManagedDebugger() override;

    void SetProtocol(Protocol *protocol) { m_protocol = protocol; }

    bool IsJustMyCode() override { return m_justMyCode; }
    void SetJustMyCode(bool enable) override { m_justMyCode = enable; }

    HRESULT Initialize() override;
    HRESULT Attach(int pid) override;
    HRESULT Launch(std::string fileExec, std::vector<std::string> execArgs, bool stopAtEntry = false) override;
    HRESULT ConfigurationDone() override;

    HRESULT Disconnect(DisconnectAction action = DisconnectDefault) override;

    int GetLastStoppedThreadId() override;

    HRESULT Continue() override;
    HRESULT Pause() override;
    HRESULT GetThreads(std::vector<Thread> &threads) override;
    HRESULT SetBreakpoints(std::string filename, const std::vector<int> &lines, std::vector<Breakpoint> &breakpoints) override;
    void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint) override;
    HRESULT GetStackTrace(int threadId, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames) override;
    HRESULT StepCommand(int threadId, StepType stepType) override;
    HRESULT GetScopes(uint64_t frameId, std::vector<Scope> &scopes) override;
    HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) override;
    int GetNamedVariables(uint32_t variablesReference) override;
    HRESULT Evaluate(uint64_t frameId, const std::string &expression, Variable &variable) override;
};

class Protocol
{
public:
    virtual void EmitInitializedEvent() = 0;
    virtual void EmitStoppedEvent(StoppedEvent event) = 0;
    virtual void EmitExitedEvent(ExitedEvent event) = 0;
    virtual void EmitTerminatedEvent() = 0;
    virtual void EmitContinuedEvent() = 0;
    virtual void EmitThreadEvent(ThreadEvent event) = 0;
    virtual void EmitModuleEvent(ModuleEvent event) = 0;
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
    std::unordered_map<std::string, std::unordered_map<int32_t, int> > m_breakpoints;
public:
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; }
    static std::string EscapeMIValue(const std::string &str);

    MIProtocol() : m_exit(false), m_varCounter(0) {}
    void EmitInitializedEvent() override {};
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override {};
    void EmitContinuedEvent() override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
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
    HRESULT SetBreakpoint(const std::string &filename, int linenum, Breakpoint &breakpoints);
    void DeleteBreakpoints(const std::unordered_set<uint32_t> &ids);
    static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
};
