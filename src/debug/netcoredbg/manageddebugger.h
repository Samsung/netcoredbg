// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "modules.h"
#include "debugger.h"
#include "protocol.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <map>
#include <condition_variable>
#include <future>

#include <list>
#include <set>


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

    std::mutex m_evalQueueMutex;
    std::list<ThreadId> m_evalQueue;

    bool is_empty_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        return m_evalQueue.empty();
    }

    void push_eval_queue(ThreadId tid) {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        m_evalQueue.push_back(tid);
    }

    ThreadId front_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        if (!m_evalQueue.empty())
            return m_evalQueue.front();
        return ThreadId{};
    }

    void pop_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        if (!m_evalQueue.empty())
            m_evalQueue.pop_front();
    }

private:

    ToRelease<ICorDebugFunction> m_pRunClassConstructor;
    ToRelease<ICorDebugFunction> m_pGetTypeHandle;
    ToRelease<ICorDebugFunction> m_pSuppressFinalize;

    struct evalResult_t {
        evalResult_t() = delete;
        evalResult_t(ICorDebugEval *pEval_, const std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > &promiseValue_) = delete;
        evalResult_t(const evalResult_t &B) = delete;
        evalResult_t& operator = (const evalResult_t &B) = delete;
        evalResult_t& operator = (evalResult_t &&B) = delete;

        evalResult_t(ICorDebugEval *pEval_, std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > &&promiseValue_) :
            pEval(pEval_),
            promiseValue(std::move(promiseValue_))
        {}
        evalResult_t(evalResult_t &&B) :
            pEval(B.pEval),
            promiseValue(std::move(B.promiseValue))
        {}

        ~evalResult_t() = default;

        ICorDebugEval *pEval;
        std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > promiseValue;
    };

    std::mutex m_evalMutex;
    std::unordered_map< DWORD, evalResult_t > m_evalResults;

    HRESULT FollowNested(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         const std::string &methodClass,
                         const std::vector<std::string> &parts,
                         ICorDebugValue **ppResult,
                         int evalFlags);
    HRESULT FollowFields(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         ICorDebugValue *pValue,
                         ValueKind valueKind,
                         const std::vector<std::string> &parts,
                         int nextPart,
                         ICorDebugValue **ppResult,
                         int evalFlags);
    HRESULT GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                       ICorDebugILFrame *pILFrame,
                                       ICorDebugValue *pInputValue,
                                       ValueKind valueKind,
                                       const std::string &name,
                                       ICorDebugValue **ppResultValue,
                                       int evalFlags);

    HRESULT WaitEvalResult(ICorDebugThread *pThread,
                           ICorDebugEval *pEval,
                           ICorDebugValue **ppEvalResult);

    HRESULT EvalObjectNoConstructor(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugValue **ppEvalResult,
        int evalFlags,
        bool suppressFinalize = true);

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

    static HRESULT FindFunction(
        ICorDebugModule *pModule,
        const WCHAR *typeName,
        const WCHAR *methodName,
        ICorDebugFunction **ppFunction);

public:

    Evaluator(Modules &modules) : m_modules(modules) {}

    HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue, int evalFlags);

    HRESULT EvalFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType *pType, // may be nullptr
        ICorDebugValue *pArgValue, // may be nullptr
        ICorDebugValue **ppEvalResult,
        int evalFlags);

    HRESULT EvalExpr(ICorDebugThread *pThread,
                     ICorDebugFrame *pFrame,
                     const std::string &expression,
                     ICorDebugValue **ppResult,
                     int evalFlags);

    bool IsEvalRunning();
    ICorDebugEval *FindEvalForThread(ICorDebugThread *pThread);

    // Should be called by ICorDebugManagedCallback
    void NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval);

    HRESULT getObjectByFunction(
        const std::string &func,
        ICorDebugThread *pThread,
        ICorDebugValue *pInValue,
        ICorDebugValue **ppOutValue,
        int evalFlags);

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

    HRESULT CreateString(
        ICorDebugThread *pThread,
        const std::string &value,
        ICorDebugValue **ppNewString);

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
        int endLine;
        ToRelease<ICorDebugBreakpoint> iCorBreakpoint;
        bool enabled;
        ULONG32 times;
        std::string condition;

        bool IsResolved() const { return modAddress != 0; }

        ManagedBreakpoint();
        ~ManagedBreakpoint();

        void ToBreakpoint(Breakpoint &breakpoint);

        ManagedBreakpoint(ManagedBreakpoint &&that) = default;
        ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
    };

    struct ManagedFunctionBreakpoint {

        struct FuncBreakpointElement {
            CORDB_ADDRESS modAddress;
            mdMethodDef methodToken;
            ToRelease<ICorDebugFunctionBreakpoint> funcBreakpoint;

            FuncBreakpointElement(CORDB_ADDRESS ma, mdMethodDef mt, ICorDebugFunctionBreakpoint *fb) :
                modAddress(ma), methodToken(mt), funcBreakpoint(fb) {}
        };

        uint32_t id;
        std::string module;
        std::string name;
        std::string params;
        ULONG32 times;
        bool enabled;
        std::string condition;
        std::vector<FuncBreakpointElement> breakpoints;

        bool IsResolved() const { return !breakpoints.empty(); }

        ManagedFunctionBreakpoint() : id(0),
                                      times(0),
                                      enabled(true)
        {}

        ~ManagedFunctionBreakpoint()
        {
            for (auto &fbel : breakpoints)
            {
                if (fbel.funcBreakpoint)
                    fbel.funcBreakpoint->Activate(0);
            }
        }

        void ToBreakpoint(Breakpoint &breakpoint) const;

        ManagedFunctionBreakpoint(ManagedFunctionBreakpoint &&that) = default;
        ManagedFunctionBreakpoint(const ManagedFunctionBreakpoint &that) = delete;
    };

    struct SourceBreakpointMapping
    {
        SourceBreakpoint breakpoint;
        uint32_t id = 0;
        std::string resolved_fullname; // if string is empty - no resolved breakpoint available in m_resolvedBreakpoints
        int resolved_linenum = 0;

        SourceBreakpointMapping() : breakpoint(0, ""), id(0), resolved_fullname(), resolved_linenum(0) {}
        ~SourceBreakpointMapping() = default;
    };

    Modules &m_modules;
    uint32_t m_nextBreakpointId;
    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, std::unordered_map<int, std::list<ManagedBreakpoint> > > m_srcResolvedBreakpoints;
    std::unordered_map<std::string, std::list<SourceBreakpointMapping> > m_srcInitialBreakpoints;

    std::unordered_map<std::string, ManagedFunctionBreakpoint > m_funcBreakpoints;
    ExceptionBreakpointStorage m_exceptionBreakpoints;

    HRESULT ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp);
    HRESULT ResolveBreakpoint(ManagedBreakpoint &bp);

    HRESULT ResolveFunctionBreakpointInModule(ICorDebugModule *pModule, ManagedFunctionBreakpoint &bp);
    HRESULT ResolveFunctionBreakpoint(ManagedFunctionBreakpoint &fbp);

    bool m_stopAtEntry;
    mdMethodDef m_entryPoint;
    ToRelease<ICorDebugFunctionBreakpoint> m_entryBreakpoint;

    void EnableOneICorBreakpointForLine(std::list<ManagedBreakpoint> &bList);
    HRESULT TrySetupEntryBreakpoint(ICorDebugModule *pModule);
    bool HitEntry(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);

    template <typename BreakpointType>
    HRESULT HandleEnabled(BreakpointType &bp, Debugger *debugger, ICorDebugThread *pThread, Breakpoint &breakpoint);

    HRESULT HitManagedBreakpoint(
        Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        mdMethodDef methodToken,
        Breakpoint &breakpoint);

    HRESULT HitManagedFunctionBreakpoint(Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        ICorDebugBreakpoint *pBreakpoint,
        mdMethodDef methodToken,
        Breakpoint &breakpoint);

public:
    Breakpoints(Modules &modules) :
        m_modules(modules), m_nextBreakpointId(1), m_stopAtEntry(false), m_entryPoint(mdMethodDefNil) {}

    HRESULT HitBreakpoint(
        Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint,
        Breakpoint &breakpoint,
        bool &atEntry);

    void DeleteAllBreakpoints();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

    HRESULT SetBreakpoints(
        ICorDebugProcess *pProcess,
        std::string filename,
        const std::vector<SourceBreakpoint> &srcBreakpoints,
        std::vector<Breakpoint> &breakpoints);

    HRESULT SetFunctionBreakpoints(
        ICorDebugProcess *pProcess,
        const std::vector<FunctionBreakpoint> &funcBreakpoints,
        std::vector<Breakpoint> &breakpoints);

    void SetStopAtEntry(bool stopAtEntry);

    HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t &output);
    HRESULT DeleteExceptionBreakpoint(const uint32_t id);
    HRESULT GetExceptionBreakMode(ExceptionBreakMode &mode, const std::string &name);
    bool MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category);
};

class Variables
{
    struct VariableReference
    {
        uint32_t variablesReference; // key
        int namedVariables;
        int indexedVariables;
        int evalFlags;

        std::string evaluateName;

        ValueKind valueKind;
        ToRelease<ICorDebugValue> value;
        FrameId frameId;

        VariableReference(const Variable &variable, FrameId frameId, ToRelease<ICorDebugValue> value, ValueKind valueKind) :
            variablesReference(variable.variablesReference),
            namedVariables(variable.namedVariables),
            indexedVariables(variable.indexedVariables),
            evalFlags(variable.evalFlags),
            evaluateName(variable.evaluateName),
            valueKind(valueKind),
            value(std::move(value)),
            frameId(frameId)
        {}

        VariableReference(uint32_t variablesReference, FrameId frameId, int namedVariables) :
            variablesReference(variablesReference),
            namedVariables(namedVariables),
            indexedVariables(0),
            evalFlags(0), // unused in this case, not involved into GetScopes routine
            valueKind(ValueIsScope),
            value(nullptr),
            frameId(frameId)
        {}

        bool IsScope() const { return valueKind == ValueIsScope; }

        VariableReference(VariableReference &&that) = default;
        VariableReference(const VariableReference &that) = delete;
    };

    Evaluator &m_evaluator;
    struct Member;

    std::unordered_map<uint32_t, VariableReference> m_variables;
    uint32_t m_nextVariableReference;

    void AddVariableReference(Variable &variable, FrameId frameId, ICorDebugValue *value, ValueKind valueKind);

public:
    HRESULT GetExceptionVariable(
        FrameId frameId,
        ICorDebugThread *pThread,
        Variable &variable);

private:
    HRESULT GetStackVariables(
        FrameId frameId,
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

    static void FixupInheritedFieldNames(std::vector<Member> &members);

    HRESULT FetchFieldsAndProperties(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        std::vector<Member> &members,
        bool fetchOnlyStatic,
        bool &hasStaticMembers,
        int childStart,
        int childEnd,
        int evalFlags);

    HRESULT GetNumChild(
        ICorDebugValue *pValue,
        unsigned int &numchild,
        bool static_members = false);

    HRESULT SetStackVariable(
        FrameId frameId,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        const std::string &name,
        const std::string &value,
        std::string &output);

    HRESULT SetChild(
        VariableReference &ref,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        const std::string &name,
        const std::string &value,
        std::string &output);

    static BOOL VarGetChild(void *opaque, uint32_t varRef, const char* name, int *typeId, void **data);
    bool GetChildDataByName(uint32_t varRef, const std::string &name, int *typeId, void **data);
    void FillValueAndType(Member &member, Variable &var, bool escape = true);

public:

    Variables(Evaluator &evaluator) :
        m_evaluator(evaluator),
        m_nextVariableReference(1)
    {}

    int GetNamedVariables(uint32_t variablesReference);

    HRESULT GetVariables(
        ICorDebugProcess *pProcess,
        uint32_t variablesReference,
        VariablesFilter filter,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT SetVariable(
        ICorDebugProcess *pProcess,
        const std::string &name,
        const std::string &value,
        uint32_t ref,
        std::string &output);

    HRESULT SetVariable(
        ICorDebugProcess *pProcess,
        ICorDebugValue *pVariable,
        const std::string &value,
        FrameId frameId,
        std::string &output);

    HRESULT GetScopes(ICorDebugProcess *pProcess,
        FrameId frameId,
        std::vector<Scope> &scopes);

    HRESULT Evaluate(ICorDebugProcess *pProcess,
        FrameId frameId,
        const std::string &expression,
        Variable &variable,
        std::string &output);

    HRESULT GetValueByExpression(
        ICorDebugProcess *pProcess,
        FrameId frameId,
        const Variable &variable,
        ICorDebugValue **ppResult);

    void Clear() { m_variables.clear(); m_nextVariableReference = 1; }

};

class ManagedCallback;

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
    ThreadId m_lastStoppedThreadId;

    std::mutex m_lastUnhandledExceptionThreadIdsMutex;
    std::set<ThreadId> m_lastUnhandledExceptionThreadIds;

    void SetLastStoppedThread(ICorDebugThread *pThread);

    std::atomic_int m_stopCounter;

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

    Modules m_modules;
    Evaluator m_evaluator;
    Breakpoints m_breakpoints;
    Variables m_variables;
    Protocol *m_protocol;
    ToRelease<ManagedCallback> m_managedCallback;
    ICorDebug *m_pDebug;
    ICorDebugProcess *m_pProcess;

    std::mutex m_stepMutex;
    std::unordered_map<DWORD, bool> m_stepSettedUp;

    bool m_justMyCode;

    std::mutex m_startupMutex;
    std::condition_variable m_startupCV;
    bool m_startupReady;
    HRESULT m_startupResult;

    PVOID m_unregisterToken;
    DWORD m_processId;
    std::string m_clrPath;

    static VOID StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr);
    HRESULT Startup(IUnknown *punk, DWORD pid);

    void Cleanup();

    static HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);

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

    void SetProtocol(Protocol *protocol) { m_protocol = protocol; }

    bool IsJustMyCode() const override { return m_justMyCode; }
    void SetJustMyCode(bool enable) override { m_justMyCode = enable; }

    HRESULT Initialize() override;
    HRESULT Attach(int pid) override;
    HRESULT Launch(const std::string &fileExec, const std::vector<std::string> &execArgs, const std::map<std::string, std::string> &env,
        const std::string &cwd, bool stopAtEntry = false) override;
    HRESULT ConfigurationDone() override;

    HRESULT Disconnect(DisconnectAction action = DisconnectDefault) override;

    ThreadId GetLastStoppedThreadId() override;
    HRESULT Continue(ThreadId threadId) override;
    HRESULT Pause() override;
    HRESULT GetThreads(std::vector<Thread> &threads) override;
    HRESULT SetBreakpoints(const std::string& filename, const std::vector<SourceBreakpoint> &srcBreakpoints, std::vector<Breakpoint> &breakpoints) override;
    HRESULT SetFunctionBreakpoints(const std::vector<FunctionBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints) override;
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

    // Functions which converts FrameId to ThreadId and FrameLevel and vice versa.
    FrameId getFrameId(ThreadId, FrameLevel);
    ThreadId threadByFrameId(FrameId) const;
    FrameLevel frameByFrameId(FrameId) const;

private:
    HRESULT Stop(ThreadId threadId, const StoppedEvent &event);
    bool MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category);
};
