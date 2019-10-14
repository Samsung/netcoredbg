// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "json/json.hpp"
#include "debugger.h"

namespace VSCodeExceptionBreakModeKeyWord
{
    static const std::string ALL = "all";
    static const std::string ALWAYS = "always";
    static const std::string NEVER = "never";
    static const std::string USERUNHANDLED = "userUnhandled";
    static const std::string USERUNHANDLED_A = "user-unhandled";
    static const std::string UNHANDLED = "unhandled";
}

class VSCodeProtocol : public Protocol
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

    uint64_t m_seqCounter;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    static std::string ReadData();

    void AddCapabilitiesTo(nlohmann::json &capabilities);
    void EmitEvent(const std::string &name, const nlohmann::json &body);
    HRESULT HandleCommand(const std::string &command, const nlohmann::json &arguments, nlohmann::json &body);

    void Log(const std::string &prefix, const std::string &text);

public:

    VSCodeProtocol() : Protocol(), m_engineLogOutput(LogNone), m_seqCounter(1) {}
    void EngineLogging(const std::string &path);
    void OverrideLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args)
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

    void EmitInitializedEvent() override;
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override;
    void EmitContinuedEvent(int threadId) override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void EmitCapabilitiesEvent();
};
