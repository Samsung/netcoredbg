// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifdef _WIN32
#include <ConsoleApi.h>
#include <ProcessEnv.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "debugger/frames.h"
#include "platform.h"
#include "torelease.h"
#include "protocols/cliprotocol.h"
#include "linenoise.h"
#include "utils/utf.h"

#include <sstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <csignal>

#include "utils/logger.h"
#include "tokenizer.h"

using namespace std::placeholders;
using std::unordered_set;
using std::string;
using std::vector;

namespace netcoredbg
{

const size_t DefaultHistoryDepth = 1024;

typedef HRESULT (CLIProtocol::*DoCommand)(const std::vector<std::string> &args, std::string &output);

CLIProtocol::TermSettings::TermSettings()
{
#ifdef WIN32
    auto in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE)
        return;

    DWORD mode;
    if (!GetConsoleMode(in, &mode))
        return;

    data.reset(reinterpret_cast<char*>(new DWORD {mode}));
    SetConsoleMode(in, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
#else
    if (!isatty(fileno(stdin)))
        return;

    struct termios ts;
    if (tcgetattr(fileno(stdin), &ts) < 0)
        return;

    data.reset(reinterpret_cast<char*>(new termios {ts}));
    ts.c_lflag &= ~ISIG;
    tcsetattr(fileno(stdin), TCSADRAIN, &ts);
#endif
}

CLIProtocol::TermSettings::~TermSettings()
{
    if (!data) return;

#ifdef WIN32
    auto in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE)
        return;
    SetConsoleMode(in, *reinterpret_cast<DWORD*>(data.get()));
#else
    tcsetattr(fileno(stdin), TCSADRAIN, reinterpret_cast<termios*>(data.get()));
#endif
}

HRESULT CLIProtocol::PrintBreakpoint(const Breakpoint &b, std::string &output)
{
    HRESULT Status;

    std::ostringstream ss;

    if (b.verified)
    {
        if(b.source.IsNull())
            ss << " Breakpoint " << b.id << " at " << b.funcname << "()";
        else
            ss << " Breakpoint " << b.id << " at " << b.source.path << ":" << b.line;
        Status = S_OK;
    }
    else if (b.source.IsNull())
    {
        ss << " Breakpoint " << b.id << " at " << b.funcname << "() --pending, warning: No executable code of the debugger's target code type is associated with this line.";
        Status = S_FALSE;
    }
    else
    {
        ss << " Breakpoint " << b.id << " at " << b.source.name << ":" << b.line << " --pending, warning: No executable code of the debugger's target code type is associated with this line.";
        Status = S_FALSE;
    }
    output = ss.str();
    return Status;
}

void CLIProtocol::EmitBreakpointEvent(BreakpointEvent event)
{
    LogFuncEntry();

    switch(event.reason)
    {
        case BreakpointChanged:
        {
            std::string output;
            PrintBreakpoint(event.breakpoint, output);
            printf("breakpoint modified, %s\n", output.c_str());
            return;
        }
        default:
            break;
    }
}

HRESULT CLIProtocol::StepCommand(const std::vector<std::string> &args,
                                std::string &output,
                                Debugger::StepType stepType)
{
    HRESULT Status;
    switch (m_processStatus) 
    {
        case Running:
        {
            ThreadId threadId{ GetIntArg(args, "--thread", int(m_debugger->GetLastStoppedThreadId())) };
            IfFailRet(m_debugger->StepCommand(threadId, stepType));
            output = "^running";
            break;
        }

        default:
            output = "No process.";
            Status = E_FAIL;
            break;
    }

    return Status;
}

HRESULT CLIProtocol::PrintFrameLocation(const StackFrame &stackFrame, std::string &output)
{
    std::ostringstream ss;

    if (!stackFrame.source.IsNull())
    {
        ss << "\n    " << stackFrame.source.path << ":" << stackFrame.line << "  (col: " << stackFrame.column << " to line: " << stackFrame.endLine << " col: " << stackFrame.endColumn << ")\n";
    }

    if (stackFrame.clrAddr.methodToken != 0)
    {
        ss << "    clr-addr: {module-id {" << stackFrame.moduleId << "}"
           << ", method-token: 0x" << std::setw(8) << std::setfill('0') << std::hex << stackFrame.clrAddr.methodToken 
           << " il-offset: " << std::dec << stackFrame.clrAddr.ilOffset << ", native offset: " << stackFrame.clrAddr.nativeOffset << "}";
    }

    ss << "\n    " << stackFrame.name;
    if (stackFrame.id != 0)
        ss << ", addr: " << IProtocol::AddrToString(stackFrame.addr);

    output = ss.str();

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT CLIProtocol::PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame)
{
    HRESULT Status;
    std::ostringstream ss;

    int totalFrames = 0;
    std::vector<StackFrame> stackFrames;
    IfFailRet(m_debugger->GetStackTrace(threadId, lowFrame, int(highFrame) - int(lowFrame), stackFrames, totalFrames));

    int currentFrame = int(lowFrame);

    ss << "stack=[";
    const char *sep = "";

    for (const StackFrame &stackFrame : stackFrames)
    {
        ss << sep;
        sep = ",";

        std::string frameLocation;
        PrintFrameLocation(stackFrame, frameLocation);

        ss << "\nframe={ level: " << currentFrame;
        if (!frameLocation.empty())
            ss << "," << frameLocation;
        ss << "\n}";
        currentFrame++;
    }

    ss << "]";

    output = ss.str();

    return S_OK;
}

void CLIProtocol::Cleanup()
{
    m_vars.clear();
    m_varCounter = 0;
    m_breakpoints.clear();
}

HRESULT CLIProtocol::SetBreakpoint(
    const std::string &filename,
    int linenum,
    const std::string &condition,
    Breakpoint &breakpoint)
{
    HRESULT Status;

    auto &breakpointsInSource = m_breakpoints[filename];
    std::vector<SourceBreakpoint> srcBreakpoints;
    for (auto it : breakpointsInSource)
        srcBreakpoints.push_back(it.second);

    srcBreakpoints.emplace_back(linenum, condition);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(m_debugger->SetBreakpoints(filename, srcBreakpoints, breakpoints));

    // Note, SetBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "srcBreakpoints".
    breakpoint = breakpoints.back();
    breakpointsInSource.insert(std::make_pair(breakpoint.id, std::move(srcBreakpoints.back())));
    return S_OK;
}

HRESULT CLIProtocol::SetFunctionBreakpoint(
    const std::string &module,
    const std::string &funcname,
    const std::string &params,
    const std::string &condition,
    Breakpoint &breakpoint)
{
    HRESULT Status;
    std::vector<FunctionBreakpoint> funcBreakpoints;

    for (const auto &it : m_funcBreakpoints)
        funcBreakpoints.push_back(it.second);

    funcBreakpoints.emplace_back(module, funcname, params, condition);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(m_debugger->SetFunctionBreakpoints(funcBreakpoints, breakpoints));

    // Note, SetFunctionBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "funcBreakpoints".
    breakpoint = breakpoints.back();
    m_funcBreakpoints.insert(std::make_pair(breakpoint.id, std::move(funcBreakpoints.back())));
    return S_OK;
}

void CLIProtocol::DeleteBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    for (auto &breakpointsIter : m_breakpoints)
    {
        std::size_t initialSize = breakpointsIter.second.size();
        std::vector<SourceBreakpoint> remainingBreakpoints;
        for (auto it = breakpointsIter.second.begin(); it != breakpointsIter.second.end();)
        {
            if (ids.find(it->first) == ids.end())
            {
                remainingBreakpoints.push_back(it->second);
                ++it;
            }
            else
                it = breakpointsIter.second.erase(it);
        }

        if (initialSize == breakpointsIter.second.size())
            continue;

        std::string filename = breakpointsIter.first;

        std::vector<Breakpoint> tmpBreakpoints;
        m_debugger->SetBreakpoints(filename, remainingBreakpoints, tmpBreakpoints);
    }
}

void CLIProtocol::DeleteFunctionBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    std::size_t initialSize = m_funcBreakpoints.size();
    std::vector<FunctionBreakpoint> remainingFuncBreakpoints;
    for (auto it = m_funcBreakpoints.begin(); it != m_funcBreakpoints.end();)
    {
        if (ids.find(it->first) == ids.end())
        {
            remainingFuncBreakpoints.push_back(it->second);
            ++it;
        }
        else
            it = m_funcBreakpoints.erase(it);
    }

    if (initialSize == m_funcBreakpoints.size())
        return;

    std::vector<Breakpoint> tmpBreakpoints;
    m_debugger->SetFunctionBreakpoints(remainingFuncBreakpoints, tmpBreakpoints);
}

void CLIProtocol::EmitStoppedEvent(StoppedEvent event)
{
    LogFuncEntry();

    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            printf("\nstopped, reason: breakpoint %i hit, thread id: %i, stopped threads: all, times= %u, frame={%s\n}\n",
                  (unsigned int)event.breakpoint.id, int(event.threadId), (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            break;
        }
        case StopStep:
        {
            printf("\nstopped, reason: end stepping range, thread id: %i, stopped threads: all, frame={%s\n}\n", int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopException:
        {
            std::string category = "clr";
            std::string stage = "unhandled";
            printf("\nstopped, reason: exception received, name: %s, exception: %s, stage: %s, category: %s, thread id: %i, stopped-threads: all, frame={%s\n}\n",
                event.text.c_str(),
                event.description.c_str(),
                stage.c_str(),
                category.c_str(),
                int(event.threadId),
                frameLocation.c_str());
            break;
        }
        case StopEntry:
        {
            printf("\nstopped, reason: entry point hit, thread id: %i, stopped threads: all, frame={%s\n}\n",
                int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopBreak:
        {
            printf("\nstopped, reason: Debugger.Break, thread id: %i, stopped threads: all, frame={%s\n}\n",
                  int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopPause:
        {
            printf("\nstopped, reason: interrupted, thread id: %i, stopped threads: all, frame={%s\n}\n",
                  int(event.threadId), frameLocation.c_str());
            break;
        }
        default:
            return;
    }
#ifndef WIN32
    pthread_kill(tid, SIGWINCH);
#endif
}

void CLIProtocol::EmitExitedEvent(ExitedEvent event)
{
    LogFuncEntry();
    m_processStatus = Exited;
    printf("\nstopped, reason: exited, exit-code: %i\n", event.exitCode);
#ifndef WIN32
    pthread_kill(tid, SIGWINCH);
#endif
}

void CLIProtocol::EmitContinuedEvent(ThreadId threadId)
{
    LogFuncEntry();
}

void CLIProtocol::EmitThreadEvent(ThreadEvent event)
{
    LogFuncEntry();

    const char *reasonText = "";
    switch(event.reason)
    {
        case ThreadStarted:
            reasonText = "thread created";
            m_processStatus = Running;
            break;
        case ThreadExited:
            reasonText = "thread exited";
            break;
    }
    printf("\n%s, id: %i\n", reasonText, int(event.threadId));
}

void CLIProtocol::EmitModuleEvent(ModuleEvent event)
{
    LogFuncEntry();

    switch(event.reason)
    {
        case ModuleNew:
        {
            std::ostringstream ss;
            std::string symload = (event.module.symbolStatus == SymbolsLoaded) ? "symbols loaded, " : "no symbols loaded, ";
            ss << event.module.path << "\n"
               << symload << "base address: 0x" << std::hex << event.module.baseAddress << ", size: " << std::dec << event.module.size
               << "(0x" << std::hex << event.module.size << ")";
            printf("\nlibrary loaded: %s\n", ss.str().c_str());
            break;
        }
        default:
            break;
    }
}

void CLIProtocol::EmitOutputEvent(OutputEvent event)
{
    LogFuncEntry();

    if (event.source.empty())
        printf("\n%s\n", event.output.c_str());
    else
        printf("\n%s, source: %s\n", event.output.c_str(), event.source.c_str());
}

HRESULT CLIProtocol::doBacktrace(const std::vector<std::string> &args_orig, std::string &output)
{
    std::vector<std::string> args = args_orig;
    ThreadId tid = m_debugger->GetLastStoppedThreadId();
    if (tid == ThreadId::AllThreads)
    {
        output ="No stack.";
        return E_FAIL;
    }
    ThreadId threadId{ GetIntArg(args, "--thread", int(tid)) };
    int lowFrame = 0;
    int highFrame = FrameLevel::MaxFrameLevel;
    StripArgs(args);
    GetIndices(args, lowFrame, highFrame);
    return PrintFrames(threadId, output, FrameLevel{lowFrame}, FrameLevel{highFrame});
}

HRESULT CLIProtocol::doBreak(const std::vector<std::string> &unmutable_args, std::string &output)
{
    HRESULT Status = E_FAIL;
    Breakpoint breakpoint;
    std::vector<std::string> args = unmutable_args;

    StripArgs(args);

    BreakType bt = GetBreakpointType(args);

    if (bt == BreakType::Error)
    {
        output = "Wrong breakpoint specified";
        return E_FAIL;
    }

    if (bt == BreakType::LineBreak)
    {
        struct LineBreak lb;

        if (ParseBreakpoint(args, lb)
            && SUCCEEDED(SetBreakpoint(lb.filename, lb.linenum, lb.condition, breakpoint)))
            Status = S_OK;
    }
    else if (bt == BreakType::FuncBreak)
    {
        struct FuncBreak fb;

        if (ParseBreakpoint(args, fb)
            && SUCCEEDED(SetFunctionBreakpoint(fb.module, fb.funcname, fb.params, fb.condition, breakpoint)))
            Status = S_OK;
    }

    if (Status == S_OK)
        PrintBreakpoint(breakpoint, output);
    else
        output = "Unknown breakpoint location format";

    return Status;
}

HRESULT CLIProtocol::doContinue(const std::vector<std::string> &, std::string &output)
{
    HRESULT Status;
    IfFailRet(m_debugger->Continue(ThreadId::AllThreads));
    output = "^running";
    return S_OK;
}

HRESULT CLIProtocol::doDelete(const std::vector<std::string> &args, std::string &)
{
    std::unordered_set<uint32_t> ids;
    for (const std::string &idStr : args)
    {
        bool ok;
        int id = ParseInt(idStr, ok);
        if (ok)
            ids.insert(id);
    }
    DeleteBreakpoints(ids);
    DeleteFunctionBreakpoints(ids);
    return S_OK;
}

HRESULT CLIProtocol::doDetach(const std::vector<std::string> &args, std::string &)
{
    m_debugger->Disconnect();
    return S_OK;
}

HRESULT CLIProtocol::doFile(const std::vector<std::string> &args, std::string &output)
{
    if (args.empty())
    {
        output = "Invalid file name";
        return E_INVALIDARG;
    }
    m_fileExec = args.at(0);
    return S_OK;
}

HRESULT CLIProtocol::doFinish(const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, Debugger::STEP_OUT);    
}

HRESULT CLIProtocol::doHelp(const std::vector<std::string> &args, std::string &output)
{
    fprintf(stdout,
        ".NET Core debugger Commands:\n"
        "\n"
        "      Command *         Shortcut               Description\n"
        "\n"
        "backtrace                  bt         Print backtrace info.\n"
        "break <file.ext:XXX>       b          Set breakpoint at source file.ext:line_number.\n"
        "break <func_name>          b          Set breakpoint at function func_name().\n"
        "continue                   c          Continue debugging after stop/pause.\n"
        "delete <X>                 d          Delete breakpoint number X.\n"
        "detach                                Detach from the debugging process.\n"
        "file <filename>                       Load executable to debug.\n"
        "finish                                Continue execution until the current stack frame returns.\n"
        "help                       h          Print this help message.\n"
        "info-thread                           Print threads info. \n"
        "interrupt                             Interrupt debugging program\n"          
        "next                       n          Step program.\n"
        "print <name>               p          Print variable's value.\n"
        "quit                       q          Quit the debugging session.\n"
        "run                        r          Start debugging program.\n"
        "step                       s          Step program until it reaches the next source line.\n"
        "set-args                              Set the debugging program arguments.\n\n"
    // todo:    "* Press <Enter> to repeat the previous command.\n\n"
    );
    return S_OK;
}

HRESULT CLIProtocol::doInfoThread(const std::vector<std::string> &, std::string &output)
{
    std::vector<Thread> threads;
    if (m_debugger->GetThreads(threads) != S_OK)
    {
        output = "No threads.";
        return E_FAIL;
    }
    std::ostringstream ss;

    ss << "\nthreads=[\n";

    const char *sep = "";
    for (const Thread& thread : threads)
    {
        ss << sep << "{id=\"" << int(thread.id)
           << "\", name=\"" << thread.name << "\", state=\""
           << (thread.running ? "running" : "stopped") << "\"}";
        sep = ",\n";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

HRESULT CLIProtocol::doInterrupt(const std::vector<std::string> &, std::string &output)
{
    HRESULT Status;
    IfFailRet(m_debugger->Pause());
    output = "^done";
    return S_OK;
}

HRESULT CLIProtocol::doNext(const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, Debugger::STEP_OVER);    
}

HRESULT CLIProtocol::PrintVariable(ThreadId threadId, FrameId frameId, std::list<std::string>::iterator token_iterator, Variable v, std::ostringstream &ss, bool expand)
{
    if(!token_iterator->empty())
    {
        token_iterator++;
    }

    bool empty = token_iterator->empty();
    ss << v.name;
    if (empty)
    {
        ss << " = " << v.value;
    } else if (token_iterator->front() != '[')
    {
        ss << ".";
    }

    if (v.namedVariables > 0 && expand)
    {
        std::vector<Variable> children;
        if (empty) 
        {
            ss << ": {";
        }
        m_debugger->GetVariables(v.variablesReference, VariablesBoth, 0, v.namedVariables, children);
        int count = 0;
        for (auto &child : children)
        {
            if (empty)
            {
                PrintVariable(threadId, frameId, token_iterator, child, ss, false);
                ss << ", ";
                count++;
            } else if (child.name == *token_iterator)
            {
                PrintVariable(threadId, frameId, token_iterator, child, ss, true);
                count++;
            }
        }
        if (count == 0)
        {
            ss << *token_iterator << " -- Not found!\n";
        }
        ss << "\b\b";
        if (empty)
        {
            ss << "}";
        }
    }
    return S_OK;
}

HRESULT CLIProtocol::doPrint(const std::vector<std::string> &args, std::string &output)
{
    if (!args.empty())
    {
        m_lastPrintArg = args[0];
    }
    else if (m_lastPrintArg.empty())
    {
        puts("The history is empty.");
        return S_OK;
    }

    std::ostringstream ss;

    HRESULT Status;
    std::string result;
    std::list<std::string> tokens;

    ss << "\n";
    ThreadId threadId = m_debugger->GetLastStoppedThreadId();
    FrameId frameId = StackFrame(threadId, FrameLevel{0}, "").id;
    Variable v(0);
    Tokenizer tokenizer(m_lastPrintArg, ".[");
    while (tokenizer.Next(result))
    {
        if (result.back() == ']')
        {
            tokens.push_back('[' + result);
        }
        else {
            tokens.push_back(result);
        }
    }
    tokens.push_back("");
    printf("\n");
    IfFailRet(m_debugger->Evaluate(frameId, tokens.front(), v, output));
    v.name = tokens.front();
    PrintVariable (threadId, frameId, tokens.begin(), v, ss, true);
    output = ss.str();
    return S_OK;
}

HRESULT CLIProtocol::doQuit(const std::vector<std::string> &, std::string &)
{
    this->m_exit = true;
    m_debugger->Disconnect(Debugger::DisconnectTerminate);
    return S_OK;
}

HRESULT CLIProtocol::doRun(const std::vector<std::string> &args, std::string &output)
{
        HRESULT Status;
        m_debugger->Initialize();
        IfFailRet(m_debugger->Launch(m_fileExec, m_execArgs, {}, "", true));
        Status = m_debugger->ConfigurationDone();
        if (SUCCEEDED(Status))
            output = "^running";
        return Status;
}

HRESULT CLIProtocol::doStep(const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, Debugger::STEP_IN);    
}

HRESULT CLIProtocol::doSetArgs(const std::vector<std::string> &args, std::string &output)
{
    m_execArgs = args;
    return S_OK;
}

HRESULT CLIProtocol::HandleCommand(const std::string& command,
                                  const std::vector<std::string> &args,
                                  std::string &output)
{
    static std::unordered_map<std::string, DoCommand> doCommands
    {
        {"backtrace", &CLIProtocol::doBacktrace},
        {"bt", &CLIProtocol::doBacktrace},
        {"b", &CLIProtocol::doBreak},
        {"break", &CLIProtocol::doBreak},
        {"c", &CLIProtocol::doContinue},
        {"continue", &CLIProtocol::doContinue},
        {"delete", &CLIProtocol::doDelete},
        {"d", &CLIProtocol::doDelete},
        {"detach", &CLIProtocol::doDetach},
        {"file", &CLIProtocol::doFile},
        {"finish", &CLIProtocol::doFinish},
        {"help", &CLIProtocol::doHelp},
        {"h", &CLIProtocol::doHelp},
        {"info-thread", &CLIProtocol::doInfoThread},
        {"interrupt", &CLIProtocol::doInterrupt},
        {"n", &CLIProtocol::doNext},
        {"next", &CLIProtocol::doNext},
        {"p", &CLIProtocol::doPrint},
        {"print", &CLIProtocol::doPrint},
        {"q", &CLIProtocol::doQuit},
        {"quit", &CLIProtocol::doQuit},
        {"r", &CLIProtocol::doRun},
        {"run", &CLIProtocol::doRun},
        {"s", &CLIProtocol::doStep},
        {"step", &CLIProtocol::doStep},
        {"set-args", &CLIProtocol::doSetArgs},
    };

    auto command_it = doCommands.find(command);
    if (command_it == doCommands.end())
    {
        output = "Unknown command: " + command;
        return E_FAIL;
    }
    return (this->*(command_it->second))(args, output);
}

bool CLIProtocol::ParseLine(const std::string &str,
                      std::string &token,
                      std::string &cmd,
                      std::vector<std::string> &args)
{
    token.clear();
    cmd.clear();
    args.clear();

    Tokenizer tokenizer(str);
    std::string result;

    if (!tokenizer.Next(result) || result.empty())
        return false;

    std::size_t i = result.find_first_not_of("0123456789");
    if (i == std::string::npos)
        return false;

    token = result.substr(0, i);
    cmd = result.substr(i);

    while (tokenizer.Next(result))
        args.push_back(result);

    return true;
}

void CLIProtocol::CommandLoop()
{
    std::string token;
    std::string input;
#ifndef WIN32
    tid = pthread_self();
#endif
    linenoiseInstallWindowChangeHandler();
    linenoiseHistorySetMaxLen(DefaultHistoryDepth);
    linenoiseHistoryLoad(history.c_str());

    while (!m_exit)
    {
        auto deleter = [](void *s) { ::free(s); };
        std::unique_ptr<char, decltype(deleter)> cmdline {nullptr, deleter};
        token.clear();
        input.clear();
        errno = 0;

        cmdline.reset(linenoise(prompt.c_str()));
        if (cmdline.get() == NULL)
        {
            if (errno != EAGAIN)
                break;
            m_debugger->Pause();
            continue;
        }

        if(m_processStatus == Exited)
        {
            m_debugger->Disconnect();
            m_processStatus = NotStarted;
        }

        input = cmdline.get();

        std::vector<std::string> args;
        std::string command;
        if (!ParseLine(input, token, command, args))
        {
            printf("%s", redOn.c_str());
            printf("%s Failed to parse input\n", token.c_str());
            printf("%s", colorOff.c_str());
            continue;
        }

        std::string output;
        HRESULT hr = HandleCommand(command, args, output);

        if (m_exit)
            break;

        if (SUCCEEDED(hr))
        {
            const char *resultClass;
            if (output.empty())
                resultClass = "^done";
            else if (output.at(0) == '^')
                resultClass = "";
            else
                resultClass = "^done,";

            printf("%s%s%s\n", token.c_str(), resultClass, output.c_str());
        }
        else
        {
            if (output.empty())
            {
                printf("%s", redOn.c_str());
                printf("%s Error: 0x%08x: %s\n", token.c_str(), hr, errormessage(hr));
                printf("%s", colorOff.c_str());
            }
            else
            {
                printf("%s", redOn.c_str());
                printf("%s %s\n", token.c_str(), output.c_str());
                printf("%s", colorOff.c_str());
            }
        }
        linenoiseHistoryAdd(cmdline.get());
    }

    if (!m_exit)
        m_debugger->Disconnect(Debugger::DisconnectTerminate);

    printf("%s^exit\n", token.c_str());
    linenoiseHistorySave(history.c_str());
    linenoiseHistoryFree();
}

} // namespace netcoredbg
