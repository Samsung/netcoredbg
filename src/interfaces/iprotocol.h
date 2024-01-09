// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "interfaces/types.h"
#include "utils/string_view.h"
#include "utils/streams.h"

namespace netcoredbg
{
using Utility::string_view;

class IDebugger;

class IProtocol
{
protected:
    std::atomic<bool> m_exit;
    std::shared_ptr<IDebugger> m_sharedDebugger;

    // File streams used to read commands and write responses.
    std::istream& cin;
    std::ostream& cout;

public:
    IProtocol(std::istream& input, std::ostream& output) : m_exit(false), m_sharedDebugger(nullptr), cin(input), cout(output)  {}
    void SetDebugger(std::shared_ptr<IDebugger> &sharedDebugger) { m_sharedDebugger = sharedDebugger; }
    virtual void EmitInitializedEvent() = 0;
    virtual void EmitExecEvent(PID, const std::string& argv0) = 0;
    virtual void EmitStoppedEvent(const StoppedEvent &event) = 0;
    virtual void EmitExitedEvent(const ExitedEvent &event) = 0;
    virtual void EmitTerminatedEvent() = 0;
    virtual void EmitContinuedEvent(ThreadId threadId) = 0;
    virtual void EmitInteropDebuggingErrorEvent(const int error_n) {}
    virtual void EmitThreadEvent(const ThreadEvent &event) = 0;
    virtual void EmitModuleEvent(const ModuleEvent &event) = 0;
    virtual void EmitOutputEvent(OutputCategory category, string_view output, string_view source = "", DWORD threadId = 0) = 0;
    virtual void EmitBreakpointEvent(const BreakpointEvent &event) = 0;
    virtual void Cleanup() = 0;
    virtual void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) = 0;
    virtual void CommandLoop() = 0;
    virtual ~IProtocol() {}
};

} // namespace netcoredbg
