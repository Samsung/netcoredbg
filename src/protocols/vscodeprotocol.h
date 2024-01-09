// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <list>
#include <condition_variable>

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

    void EmitMessage(nlohmann::json &message, std::string &output);
    void EmitMessageWithLog(const std::string &message_prefix, nlohmann::json &message);
    void EmitEvent(const std::string &name, const nlohmann::json &body);

    void Log(const std::string &prefix, const std::string &text);

    struct CommandQueueEntry
    {
        std::string command;
        nlohmann::json arguments;
        nlohmann::json response;
    };

    std::mutex m_commandsMutex;
    std::condition_variable m_commandsCV;
    std::condition_variable m_commandSyncCV;
    std::list<CommandQueueEntry> m_commandsQueue;

    void CommandsWorker();
    std::list<CommandQueueEntry>::iterator CancelCommand(const std::list<CommandQueueEntry>::iterator &iter);

public:

    VSCodeProtocol(std::istream& input, std::ostream& output) :
        IProtocol(input, output), m_engineLogOutput(LogNone), m_seqCounter(1) {}
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
    void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "", DWORD threadId = 0) override;
    void EmitBreakpointEvent(const BreakpointEvent &event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void EmitCapabilitiesEvent();
};

} // namespace netcoredbg
