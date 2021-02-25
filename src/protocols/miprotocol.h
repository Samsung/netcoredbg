// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/escaped_string.h"
#include "debugger/debugger.h"
#include "iprotocol.h"

namespace netcoredbg
{

class MIProtocol : public IProtocol
{
    static std::mutex m_outMutex;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    unsigned int m_varCounter;
    std::unordered_map<std::string, Variable> m_vars;
    std::unordered_map<std::string, std::unordered_map<uint32_t, SourceBreakpoint> > m_breakpoints;
    std::unordered_map<uint32_t, FunctionBreakpoint> m_funcBreakpoints;


    struct MIProtocolChars;
    using EscapeMIValue = EscapedString<MIProtocolChars>;


    static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output);
    static void PrintVar(const std::string &varobjName, Variable &v, ThreadId threadId, int print_values, std::string &output);
#ifdef _MSC_VER
    static void Printf(_Printf_format_string_ const char *fmt, ...);
#else
    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));
#endif

    static bool IsEditable(const std::string &type);

public:

    MIProtocol() : IProtocol(), m_varCounter(0) {}
    void EmitInitializedEvent() override {}
    void EmitExecEvent(PID, const std::string& argv0) override {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override {}
    void EmitContinuedEvent(ThreadId threadId) override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args)
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

private:
    HRESULT HandleCommand(const std::string& command,
                          const std::vector<std::string> &args,
                          std::string &output);

    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
    HRESULT PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame);
    HRESULT PrintVariables(const std::vector<Variable> &variables, std::string &output);
    HRESULT CreateVar(ThreadId threadId, FrameLevel level, int evalFlags, const std::string &varobjName, const std::string &expression, std::string &output);
    HRESULT DeleteVar(const std::string &varobjName);
    HRESULT FindVar(const std::string &varobjName, Variable &variable);
    void PrintChildren(std::vector<Variable> &children, ThreadId threadId, int print_values, bool has_more, std::string &output);
    void PrintNewVar(const std::string& varobjName, Variable &v, ThreadId threadId, int print_values, std::string &output);
    HRESULT ListChildren(ThreadId threadId, FrameLevel level, int childStart, int childEnd, const std::string &varName, int print_values, std::string &output);
    HRESULT SetBreakpoint(const std::string &filename, int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFunctionBreakpoint(const std::string &module, const std::string &funcname, const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT SetBreakpointCondition(uint32_t id, const std::string &condition);
    HRESULT SetFunctionBreakpointCondition(uint32_t id, const std::string &condition);
    void DeleteBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteFunctionBreakpoints(const std::unordered_set<uint32_t> &ids);
    static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
    HRESULT InsertExceptionBreakpoints(const ExceptionBreakMode &mode, const std::vector<std::string>& names, std::string &output);
    HRESULT DeleteExceptionBreakpoints(const std::unordered_set<uint32_t> &ids, std::string &output);
    bool MIProtocol::ParseLine(const std::string &str, std::string &token, std::string &cmd, std::vector<std::string> &args);
};

} // namespace netcoredbg
