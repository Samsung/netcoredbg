// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>
#include <vector>

// For `HRESULT` definition
#include <pal_mstypes.h>

#include "protocol.h"

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
    virtual HRESULT Launch(std::string fileExec, std::vector<std::string> execArgs, bool stopAtEntry = false) = 0;
    virtual HRESULT ConfigurationDone() = 0;

    virtual HRESULT Disconnect(DisconnectAction action = DisconnectDefault) = 0;

    virtual int GetLastStoppedThreadId() = 0;

    virtual HRESULT Continue() = 0;
    virtual HRESULT Pause() = 0;
    virtual HRESULT GetThreads(std::vector<Thread> &threads) = 0;
    virtual HRESULT SetBreakpoints(std::string filename, const std::vector<int> &lines, std::vector<Breakpoint> &breakpoints) = 0;
    virtual void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint) = 0;
    virtual HRESULT GetStackTrace(int threadId, int startFrame, int levels, std::vector<StackFrame> &stackFrames, int &totalFrames) = 0;
    virtual HRESULT StepCommand(int threadId, StepType stepType) = 0;
    virtual HRESULT GetScopes(uint64_t frameId, std::vector<Scope> &scopes) = 0;
    virtual HRESULT GetVariables(uint32_t variablesReference, VariablesFilter filter, int start, int count, std::vector<Variable> &variables) = 0;
    virtual int GetNamedVariables(uint32_t variablesReference) = 0;
    virtual HRESULT Evaluate(uint64_t frameId, const std::string &expression, Variable &variable) = 0;
};

class Protocol
{
protected:
    bool m_exit;
    Debugger *m_debugger;

public:
    Protocol() : m_exit(false), m_debugger(nullptr) {}
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; }

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
