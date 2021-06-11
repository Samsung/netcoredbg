// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <fstream>
#include <mutex>
#include <string>

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtautological-overlap-compare"
#include "json/json.hpp"
#pragma GCC diagnostic pop

#include "interfaces/iprotocol.h"

namespace netcoredbg
{

class VSCodeProtocol : public IProtocol
{
    static const std::string TWO_CRLF;
    static const std::string CONTENT_LENGTH;

    static const std::string LOG_COMMAND;
    static const std::string LOG_RESPONSE;
    static const std::string LOG_EVENT;

    std::mutex m_outMutex;
    enum {
        LogNone,
        LogConsole,
        LogFile
    } m_engineLogOutput;
    std::ofstream m_engineLog;
    uint64_t m_seqCounter; // Note, this counter must be covered by m_outMutex.

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    std::string ReadData();

    void AddCapabilitiesTo(nlohmann::json &capabilities);
    void EmitEvent(const std::string &name, const nlohmann::json &body);
    HRESULT HandleCommand(const std::string &command, const nlohmann::json &arguments, nlohmann::json &body);

    void Log(const std::string &prefix, const std::string &text);

public:

    VSCodeProtocol(std::istream& input, std::ostream& output) : IProtocol(input, output), m_engineLogOutput(LogNone), m_seqCounter(1) {}
    void EngineLogging(const std::string &path);
    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) override
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

    void EmitInitializedEvent() override;
    void EmitExecEvent(PID, const std::string& argv0) override;
    void EmitStoppedEvent(const StoppedEvent &event) override;
    void EmitExitedEvent(const ExitedEvent &event) override;
    void EmitTerminatedEvent() override;
    void EmitContinuedEvent(ThreadId threadId) override;
    void EmitThreadEvent(const ThreadEvent &event) override;
    void EmitModuleEvent(const ModuleEvent &event) override;
    void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "") override;
    void EmitBreakpointEvent(const BreakpointEvent &event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void EmitCapabilitiesEvent();
};

} // namespace netcoredbg
