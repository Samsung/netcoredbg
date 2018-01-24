// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <mutex>

#include "json.hpp"
#include "debugger.h"


class VSCodeProtocol : public Protocol
{
    static const std::string TWO_CRLF;
    static const std::string CONTENT_LENGTH;

    std::mutex m_outMutex;
    bool m_exit;
    Debugger *m_debugger;

    uint64_t m_seqCounter;

    static std::string ReadData();

    void EmitEvent(const std::string &name, const nlohmann::json &body);
    HRESULT HandleCommand(const std::string &command, const nlohmann::json &arguments, nlohmann::json &body);
public:

    VSCodeProtocol() : m_exit(false), m_seqCounter(1) {}
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; }
    void EmitInitializedEvent() override;
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override;
    void EmitContinuedEvent() override {}
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;
};
