// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>

#include "debugger.h"
#include "iprotocol.h"


class CLIProtocol : public IProtocol
{
    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    unsigned int m_varCounter;
    std::unordered_map<std::string, Variable> m_vars;
    std::unordered_map<std::string, std::unordered_map<uint32_t, SourceBreakpoint> > m_breakpoints;
    std::unordered_map<uint32_t, FunctionBreakpoint> m_funcBreakpoints;
    std::string prompt;
    std::string history;
    std::string redOn;
    std::string colorOff;
#ifndef WIN32
    pthread_t tid;
#endif
    static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output);
    
public:

    CLIProtocol() : IProtocol(), m_varCounter(0), prompt("\x1b[1;32mcli\x1b[0m> "), history(".history"),
#ifndef WIN32
                                 redOn("\033[1;31m"), colorOff("\033[0m") {}
#else
                                 redOn(""), colorOff("") {}
#endif                                
    void EmitInitializedEvent() override {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override {}
    void EmitContinuedEvent(int threadId) override;
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
    HRESULT doBacktrace(const std::vector<std::string> &args, std::string &output);
    HRESULT doBreak(const std::vector<std::string> &args, std::string &output);
    HRESULT doContinue(const std::vector<std::string> &, std::string &output);
    HRESULT doDelete(const std::vector<std::string> &args, std::string &);
    HRESULT doFile(const std::vector<std::string> &args, std::string &);
    HRESULT doFinish(const std::vector<std::string> &args, std::string &output);
    HRESULT doHelp(const std::vector<std::string> &args, std::string &output);
    HRESULT doNext(const std::vector<std::string> &args, std::string &output);
    HRESULT doPrint(const std::vector<std::string> &args, std::string &output);
    HRESULT doQuit(const std::vector<std::string> &, std::string &);
    HRESULT doRun(const std::vector<std::string> &args, std::string &output);
    HRESULT doStep(const std::vector<std::string> &args, std::string &output);
    HRESULT doSetArgs(const std::vector<std::string> &args, std::string &output);
    HRESULT HandleCommand(std::string command,
                          const std::vector<std::string> &args,
                          std::string &output);

    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
    HRESULT PrintFrames(int threadId, std::string &output, int lowFrame, int highFrame);
    HRESULT SetBreakpoint(const std::string &filename, int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFunctionBreakpoint(const std::string &module, const std::string &funcname, const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT PrintVariable(int threadId, uint64_t frameId, std::list<std::string>::iterator it, Variable v, std::ostringstream &output, bool expand);
    void DeleteBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteFunctionBreakpoints(const std::unordered_set<uint32_t> &ids);
    static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
    bool ParseLine(const std::string &str, std::string &token, std::string &cmd, std::vector<std::string> &args);

    std::string m_lastPrintArg;
};
