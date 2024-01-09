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
#include "protocols/escaped_string.h"
#include "protocols/protocol_utils.h"
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
    void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "", DWORD threadId = 0) override;
    void EmitBreakpointEvent(const BreakpointEvent &event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) override
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

    struct MIVariable
    {
        Variable variable;
        ThreadId threadId;
        FrameLevel level;
    };

    class VariablesHandle
    {
    private:
        std::unordered_map<std::string, MIVariable> m_vars;

    public:
        HRESULT CreateVar(std::shared_ptr<IDebugger> &sharedDebugger, ThreadId threadId, FrameLevel level, int evalFlags,
                          const std::string &varobjName, const std::string &expression, std::string &output);
        HRESULT DeleteVar(const std::string &varobjName);
        HRESULT FindVar(const std::string &varobjName, MIVariable &variable);
        HRESULT PrintChildren(std::vector<Variable> &children, ThreadId threadId, FrameLevel level, int print_values, bool has_more, std::string &output);
        HRESULT PrintNewVar(const std::string& varobjName, Variable &v, ThreadId threadId, FrameLevel level, int print_values, std::string &output);
        HRESULT ListChildren(std::shared_ptr<IDebugger> &sharedDebugger, int childStart, int childEnd,
                             const MIVariable &miVariable, int print_values, std::string &output);
        void Cleanup();
    };

private:

    std::mutex m_outMutex;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    VariablesHandle m_variablesHandle;
    BreakpointsHandle m_breakpointsHandle;

#ifdef _MSC_VER
    void Printf(_Printf_format_string_ const char *fmt, ...);
#else
    void Printf(const char *fmt, ...) __attribute__((format (printf, 2, 3)));
#endif
};

} // namespace netcoredbg
