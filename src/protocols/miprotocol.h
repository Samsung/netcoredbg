// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iostream>
#include <memory>
#include "utils/string_view.h"
#include "utils/escaped_string.h"
#include "interfaces/idebugger.h"
#include "interfaces/iprotocol.h"

namespace netcoredbg
{

using Utility::string_view;

class MIProtocol : public IProtocol
{
public:

    struct MIProtocolChars;
    using EscapeMIValue = EscapedString<MIProtocolChars>;

    MIProtocol(std::istream& input, std::ostream& output) : IProtocol(input, output) {}

    void EmitInitializedEvent() override {}
    void EmitExecEvent(PID, const std::string& argv0) override {}
    void EmitStoppedEvent(const StoppedEvent &event) override;
    void EmitExitedEvent(const ExitedEvent &event) override;
    void EmitTerminatedEvent() override {}
    void EmitContinuedEvent(ThreadId threadId) override;
    void EmitThreadEvent(const ThreadEvent &event) override;
    void EmitModuleEvent(const ModuleEvent &event) override;
    void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "") override;
    void EmitBreakpointEvent(const BreakpointEvent &event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) override
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

private:

    std::mutex m_outMutex;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    std::unordered_map<std::string, Variable> m_vars;
    std::unordered_map<std::string, std::unordered_map<uint32_t, LineBreakpoint> > m_lineBreakpoints;
    std::unordered_map<uint32_t, FuncBreakpoint> m_funcBreakpoints;

#ifdef _MSC_VER
    void Printf(_Printf_format_string_ const char *fmt, ...);
#else
    void Printf(const char *fmt, ...) __attribute__((format (printf, 2, 3)));
#endif

    HRESULT HandleCommand(const std::string& command, const std::vector<std::string> &args, std::string &output);
    HRESULT StepCommand(const std::vector<std::string> &args, std::string &output, IDebugger::StepType stepType);
    HRESULT PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame);
    HRESULT CreateVar(ThreadId threadId, FrameLevel level, int evalFlags, const std::string &varobjName, const std::string &expression, std::string &output);
    HRESULT DeleteVar(const std::string &varobjName);
    HRESULT FindVar(const std::string &varobjName, Variable &variable);
    HRESULT PrintChildren(std::vector<Variable> &children, ThreadId threadId, int print_values, bool has_more, std::string &output);
    HRESULT PrintNewVar(const std::string& varobjName, Variable &v, ThreadId threadId, int print_values, std::string &output);
    HRESULT ListChildren(ThreadId threadId, FrameLevel level, int childStart, int childEnd, const std::string &varName, int print_values, std::string &output);
    HRESULT SetLineBreakpoint(const std::string &module, const std::string &filename, int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFuncBreakpoint(const std::string &module, const std::string &funcname, const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT SetLineBreakpointCondition(uint32_t id, const std::string &condition);
    HRESULT SetFuncBreakpointCondition(uint32_t id, const std::string &condition);
    void DeleteLineBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteFuncBreakpoints(const std::unordered_set<uint32_t> &ids);
    HRESULT InsertExceptionBreakpoints(const ExceptionBreakMode &mode, const std::vector<std::string>& names, std::string &output);
    HRESULT DeleteExceptionBreakpoints(const std::unordered_set<uint32_t> &ids, std::string &output);
};

} // namespace netcoredbg
