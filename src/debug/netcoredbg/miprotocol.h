// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <mutex>

#include "debugger.h"


class MIProtocol : public Protocol
{
    static std::mutex m_outMutex;
    bool m_exit;
    Debugger *m_debugger;

    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    unsigned int m_varCounter;
    std::unordered_map<std::string, Variable> m_vars;
    std::unordered_map<std::string, std::unordered_map<int32_t, int> > m_breakpoints;
public:
    void SetDebugger(Debugger *debugger) { m_debugger = debugger; }
    static std::string EscapeMIValue(const std::string &str);

    MIProtocol() : m_exit(false), m_varCounter(0) {}
    void EmitInitializedEvent() override {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override {}
    void EmitContinuedEvent() override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;

    static void Printf(const char *fmt, ...) __attribute__((format (printf, 1, 2)));

private:
    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
    HRESULT PrintFrames(int threadId, std::string &output, int lowFrame, int highFrame);
    HRESULT PrintVariables(const std::vector<Variable> &variables, std::string &output);
    HRESULT CreateVar(int threadId, int level, const std::string &varobjName, const std::string &expression, std::string &output);
    HRESULT DeleteVar(const std::string &varobjName);
    void PrintChildren(std::vector<Variable> &children, int threadId, int print_values, bool has_more, std::string &output);
    void PrintNewVar(std::string varobjName, Variable &v, int threadId, int print_values, std::string &output);
    HRESULT ListChildren(int threadId, int level, int childStart, int childEnd, const std::string &varName, int print_values, std::string &output);
    HRESULT SetBreakpoint(const std::string &filename, int linenum, Breakpoint &breakpoints);
    void DeleteBreakpoints(const std::unordered_set<uint32_t> &ids);
    static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
};
