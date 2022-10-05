// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>
#include <memory>
#include "interfaces/types.h"
#include "utils/string_view.h"
#include "utils/streams.h"

namespace netcoredbg
{
using Utility::string_view;

class IProtocol;

class IDebugger
{
protected:
    std::shared_ptr<IProtocol> m_sharedProtocol;

public:

    enum StepType
    {
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

    // This is lightweight structure which carry breakpoint information.
    struct BreakpointInfo
    {
        unsigned    id;
        bool        resolved;
        bool        enabled;
        unsigned    hit_count;
        std::string condition; // not empty for conditional breakpoints
        std::string name;      // file name or function name, depending on type.
        int         line;      // first line, 0 for function breakpoint
        int         last_line;
        std::string module;    // module name
        std::string funcsig;   // might be non-empty for function breakpoints

        bool operator<(const BreakpointInfo& other) const { return id < other.id; }
        bool operator==(const BreakpointInfo& other) const { return id == other.id; }
    };

    enum class AsyncResult
    {
        Canceled,   // function canceled due to debugger interruption
        Error,      // IO error
        Eof         // EOF reached
    };
    virtual IDebugger::AsyncResult ProcessStdin(InStream &) { return IDebugger::AsyncResult::Eof; }


    void SetProtocol(std::shared_ptr<IProtocol> &sharedProtocol)  { m_sharedProtocol = sharedProtocol; }
    virtual ~IDebugger() {}
    virtual bool IsJustMyCode() const = 0;
    virtual void SetJustMyCode(bool enable) = 0;
    virtual bool IsStepFiltering() const = 0;
    virtual void SetStepFiltering(bool enable) = 0;
    virtual bool IsHotReload() const = 0;
    virtual HRESULT SetHotReload(bool enable) = 0;
    virtual HRESULT Initialize() = 0;
    virtual HRESULT Attach(int pid) = 0;
    virtual HRESULT Launch(const std::string &fileExec, const std::vector<std::string> &execArgs, const std::map<std::string, std::string> &env,
        const std::string &cwd, bool stopAtEntry = false) = 0;
    virtual HRESULT ConfigurationDone() = 0;
    virtual HRESULT Disconnect(DisconnectAction action = DisconnectDefault) = 0;
    virtual ThreadId GetLastStoppedThreadId() = 0;
    virtual HRESULT Continue(ThreadId threadId) = 0;
    virtual HRESULT Pause(ThreadId lastStoppedThread) = 0;
    virtual HRESULT GetThreads(std::vector<Thread> &threads) = 0;
    virtual HRESULT SetLineBreakpoints(const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints) = 0;
    virtual HRESULT SetFuncBreakpoints(const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints) = 0;
    virtual HRESULT SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints) = 0;
    virtual HRESULT BreakpointActivate(int id, bool act) = 0;
    virtual void EnumerateBreakpoints(std::function<bool (const BreakpointInfo&)>&& callback) = 0;
    virtual HRESULT AllBreakpointsActivate(bool act) = 0;
    virtual HRESULT GetStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames, bool hotReloadAwareCaller = false) = 0;
    virtual HRESULT StepCommand(ThreadId threadId, StepType stepType) = 0;
    virtual HRESULT GetScopes(FrameId frameId, std::vector<Scope> &scopes) = 0;
    virtual HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) = 0;
    virtual int GetNamedVariables(uint32_t variablesReference) = 0;
    virtual HRESULT Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output) = 0;
    virtual void CancelEvalRunning() = 0;
    virtual HRESULT SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output) = 0;
    virtual HRESULT SetExpression(FrameId frameId, const std::string &expression, int evalFlags, const std::string &value, std::string &output) = 0;
    virtual HRESULT GetExceptionInfo(ThreadId threadId, ExceptionInfo &exceptionInfo) = 0;
    virtual HRESULT GetSourceFile(const std::string &sourcePath, char** fileBuf, int* fileLen) = 0;
    virtual void FreeUnmanaged(PVOID mem) = 0;
    virtual HRESULT HotReloadApplyDeltas(const std::string &dllFileName, const std::string &deltaMD, const std::string &deltaIL,
                                         const std::string &deltaPDB, const std::string &lineUpdates) = 0;
    typedef std::function<void(const char *)> SearchCallback;
    virtual void FindFileNames(string_view pattern, unsigned limit, SearchCallback) = 0;
    virtual void FindFunctions(string_view pattern, unsigned limit, SearchCallback) = 0;
    virtual void FindVariables(ThreadId, FrameLevel, string_view, unsigned limit, SearchCallback) = 0;
};

} // namespace netcoredbg
