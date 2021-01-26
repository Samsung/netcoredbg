// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include "string_view.h"

// For `HRESULT` definition
#ifdef FEATURE_PAL
#include <pal_mstypes.h>
#else
#include <windows.h>
#endif

#include "protocols/protocol.h"
#include "string_view.h"

namespace netcoredbg
{
using Utility::string_view;

using Utility::string_view;

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

    virtual bool IsJustMyCode() const = 0;
    virtual void SetJustMyCode(bool enable) = 0;

    virtual HRESULT Initialize() = 0;
    virtual HRESULT Attach(int pid) = 0;
    virtual HRESULT Launch(const std::string &fileExec, const std::vector<std::string> &execArgs, const std::map<std::string, std::string> &env,
        const std::string &cwd, bool stopAtEntry = false) = 0;
    virtual HRESULT ConfigurationDone() = 0;

    virtual HRESULT Disconnect(DisconnectAction action = DisconnectDefault) = 0;

    virtual ThreadId GetLastStoppedThreadId() = 0;

    virtual HRESULT Continue(ThreadId threadId) = 0;
    virtual HRESULT Pause() = 0;
    virtual HRESULT GetThreads(std::vector<Thread> &threads) = 0;
    virtual HRESULT SetBreakpoints(const std::string& filename, const std::vector<SourceBreakpoint> &srcBreakpoints, std::vector<Breakpoint> &breakpoints) = 0;
    virtual HRESULT SetFunctionBreakpoints(const std::vector<FunctionBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints) = 0;
    virtual HRESULT GetStackTrace(ThreadId threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames) = 0;
    virtual HRESULT StepCommand(ThreadId threadId, StepType stepType) = 0;
    virtual HRESULT GetScopes(FrameId frameId, std::vector<Scope> &scopes) = 0;
    virtual HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) = 0;
    virtual int GetNamedVariables(uint32_t variablesReference) = 0;
    virtual HRESULT Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output) = 0;
    virtual HRESULT SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output) = 0;
    virtual HRESULT SetVariableByExpression(FrameId frameId, const Variable &variable, const std::string &value, std::string &output) = 0;
    virtual HRESULT GetExceptionInfoResponse(ThreadId threadId, ExceptionInfoResponse &exceptionResponse) = 0;
    virtual HRESULT DeleteExceptionBreakpoint(const uint32_t id) = 0;
    virtual HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string& names, uint32_t &id) = 0;

    typedef std::function<void(const char *)> SearchCallback;
    virtual void FindFileNames(string_view pattern, unsigned limit, SearchCallback) = 0;
    virtual void FindFunctions(string_view pattern, unsigned limit, SearchCallback) = 0;
    virtual void FindVariables(ThreadId, FrameLevel, string_view, unsigned limit, SearchCallback) = 0;
};

class Protocol
{
protected:
    bool m_exit;
    Debugger *m_debugger;

    // File streams used to read commands and write responses.
    std::istream& cin;
    std::ostream& cout;

public:
    Protocol(std::istream& input, std::ostream& output) : m_exit(false), m_debugger(nullptr), cin(input), cout(output)  {}
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; }

    virtual void EmitInitializedEvent() = 0;
    virtual void EmitExecEvent(PID, const std::string& argv0) = 0;
    virtual void EmitStoppedEvent(StoppedEvent event) = 0;
    virtual void EmitExitedEvent(ExitedEvent event) = 0;
    virtual void EmitTerminatedEvent() = 0;
    virtual void EmitContinuedEvent(ThreadId threadId) = 0;
    virtual void EmitThreadEvent(ThreadEvent event) = 0;
    virtual void EmitModuleEvent(ModuleEvent event) = 0;
    virtual void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "") = 0;
    virtual void EmitBreakpointEvent(BreakpointEvent event) = 0;
    virtual void Cleanup() = 0;
    virtual void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) = 0;
    virtual void CommandLoop() = 0;
    virtual ~Protocol() {}
};

} // namespace netcoredbg
