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
#include "filesystem.h"

#include "limits.h"
#include <sstream>
#include <functional>
#include <algorithm>
#include <numeric>
#include <memory>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <csignal>
#include "limits.h"

#include "string_view.h"
#include "span.h"
#include "utils/logger.h"
#include "tokenizer.h"

#include "tty.h"
#include "completions.h"


// Each protocol receives input/output streams as arguments and should use these streams
// for receiving commands and printing responses. Standard IO streams should not be used
// for this purpose.
#undef stdout
#define stdout "standard IO files shouldn't be used as protocol input/output streams"
#undef stderr
#define stderr "standard IO files shouldn't be used as protocol input/output streams"

// Check printf arguments and call printf_checked() function which will print
// not to stdout, but to `cout` stream, which was passed to CLIProtocol constructor.
#define printf(fmt, ...) (false ? printf(fmt, ##__VA_ARGS__) : printf_checked(fmt, ##__VA_ARGS__))

#ifdef _MSC_VER
// This should never be called and need only to avoid warning from Visual Studio.
static int printf_checked(const char *, ...) { return -1; }
#endif


namespace netcoredbg
{

using std::unordered_set;
using std::string;
using std::vector;
using Utility::literal;
using CommandTag = CLIProtocol::CommandTag;
using CompletionTag = CLIProtocol::CompletionTag;

// Prompt which displayed when netcoredbg expects next command:
const auto Prompt = tty::bold + tty::green + literal("cli>") + tty::reset + literal(" ");

const char HistoryFileName[] = ".netcoredbg_hist";
const size_t DefaultHistoryDepth = 1024;

// Tags for all commands (including compound ones, like "info breakpoints")
// known to the command interpreter.
enum class CLIProtocol::CommandTag
{
    Help,
    Backtrace,
    Break,
    Continue,
    Delete,
    Detach,
    File,
    Finish,
    Interrupt,
    Next,
    Print,
    Quit,
    Run,
    Step,
    Source,

    // set subcommands
    Set,
    SetArgs,
    SetHelp,

    // info subcommand
    Info,
    InfoThreads,
    InfoBreakpoints,
    InfoHelp,

    // help subcommands
    HelpInfo,
    HelpSet,

    // These two definitons should end command list.
    CommandsCount,  // Total number of the commands.
    End             // Not a command, but special marker for end of list.
};

// Tags for completion types (functions).
enum class CLIProtocol::CompletionTag
{
    Command,            // this element must present always (to complete command names).

    Break,
    Delete,
    File,
    Print,

    CompletionsCount    // Total number of tags, should be last.
};


// This structure holds description of all known (sub)commands.
struct CLIProtocol::CommandsList
{

struct Aux
{
    string_view args;   // arguments list (for help)
    string_view help;   // help string
};

using CLIParams = CLIHelperParams<
    Aux,
    CLIProtocol::CommandTag, CLIProtocol::BindHandler,
    CLIProtocol::CompletionTag, CLIProtocol::BindCompletions>;

// Subcommands for "info" command.
constexpr static const CLIParams::CommandInfo info_commands[] =
{
    {CommandTag::InfoThreads,    {}, {}, {{"threads"}}, {{}, "Display currently known threads."}},
    {CommandTag::InfoBreakpoints,{}, {}, {{"breakpoints", "break"}}, {{}, "Display existing breakpoints."}},
    {CommandTag::InfoHelp,       {}, {}, {{"help"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// Subcommands for "set" command.
constexpr static const CLIParams::CommandInfo set_commands[] =
{
    {CommandTag::SetArgs, {}, {}, {{"args"}},
            {{"[args...]"}, "Set argument list to give program being debugged\n"
                            "when it is started (via 'run' command)."}},

    {CommandTag::SetHelp, {}, {}, {{"help"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// Subcommands for "help" command.
constexpr static const CLIParams::CommandInfo help_commands[] =
{
    {CommandTag::HelpInfo, {}, {}, {{"info"}}, {{}, {}}},
    {CommandTag::HelpSet,  {}, {}, {{"set"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// List of currently implemented commands. Help message will be printed in same order.
constexpr static const CLIParams::CommandInfo commands_list[] =
{
    {CommandTag::Backtrace, {}, {}, {{"backtrace", "bt"}},
        {{}, "Print backtrace info."}},

    {CommandTag::Break, {}, {{{1, CompletionTag::Break}}}, {{"break", "b"}},
        {"<loc>", "Set breakpoint at specified location, where the\n"
                        "location might be filename.cs:line or function name."}},

    {CommandTag::Continue, {}, {}, {{"continue", "c"}},
        {{}, "Continue debugging after stop/pause."}},

    {CommandTag::Delete, {}, {{{1, CompletionTag::Delete}}}, {{"delete", "clear"}},
        {"<num>", "Delete breakpoint with specified number."}},

    {CommandTag::Detach, {}, {}, {{"detach"}},
        {{}, "Detach from the debugged process."}},

    {CommandTag::File, {}, {{{1, CompletionTag::File}}}, {{"file"}},
        {"<file>", "load executable file to debug."}},

    {CommandTag::Finish, {}, {}, {{"finish"}},
        {{}, "Continue execution till end of the current function."}},

    {CommandTag::Interrupt, {}, {}, {{"interrupt"}},
        {{}, "Interrupt program execution, stop all threads."}},

    {CommandTag::Next, {}, {}, {{"next", "n"}},
        {{}, "Step program, through function calls."}},

    {CommandTag::Print, {}, {{{1, CompletionTag::Print}}}, {{"print", "p"}},
        {"<expr>", "Print variable value or evaluate an expression."}},

    {CommandTag::Quit, {}, {}, {{"quit" /*, "q" TODO: ask confirmation */}},
        {{}, "Quit the debugger."}},

    {CommandTag::Run, {}, {}, {{"run", "r"}},
        {{}, "Start debugged program."}},

    {CommandTag::Step, {}, {}, {{"step", "s"}},
        {{}, "Step program until a different source line."}},

    {CommandTag::Source, {}, {{{1, CompletionTag::File}}}, {{"source"}},
        {"<file>", "Read commands from a file."}},

    {CommandTag::Set, set_commands, {}, {{"set"}},
        {"args...", "Set miscellaneous options (see 'help set')"}},

    {CommandTag::Info, info_commands, {}, {{"info"}},
        {"<topic>", "Show misc. things about the program being debugged."}},

    {CommandTag::Help, help_commands, {}, {{"help"}},
        {"[topic]", "Show help on specified topic or print\n"
                    "this help message (if no argument specified)."}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// This class parses commands and provides calls to functions
// which handles commands and completions...
static const CLIHelper<CLIParams> cli_helper;

}; // CLIProtocol::CommandsList class


// reserve memory for static members
constexpr const CLIProtocol::CLIParams::CommandInfo CLIProtocol::CommandsList::commands_list[];
constexpr const CLIProtocol::CLIParams::CommandInfo CLIProtocol::CommandsList::help_commands[];
constexpr const CLIProtocol::CLIParams::CommandInfo CLIProtocol::CommandsList::info_commands[];
constexpr const CLIProtocol::CLIParams::CommandInfo CLIProtocol::CommandsList::set_commands[];

// instantiate cli_helper class which allows to parse command line, dispatch
// appropriate command or perform command completons
const CLIHelper<CLIProtocol::CommandsList::CLIParams>
CLIProtocol::CommandsList::cli_helper{CLIProtocol::CommandsList::commands_list};

// Just forward declaration.
template <>
HRESULT CLIProtocol::printHelp<CLIProtocol::CLIParams::CommandInfo>(
        const CLIProtocol::CLIParams::CommandInfo *, string_view args);


// This class reads input lines from console.
class ConsoleLineReader : public CLIProtocol::LineReader
{
public:
    ConsoleLineReader() : cmdline(nullptr, deleter) {}

    virtual std::tuple<string_view, Result> get_line(const char *prompt) override
    {
        errno = 0;
        cmdline.reset(linenoise(prompt));
        if (!cmdline)
            return {string_view{}, errno == EAGAIN ? Interrupt : Eof};

        linenoiseHistoryAdd(cmdline.get());
        return {string_view{cmdline.get()}, Success};
    }

private:
    static void deleter(void *s) { ::free(s); };
    std::unique_ptr<char, decltype(&deleter)> cmdline;
};

// This class reads lines from arbitrary input stream (file, pipe, etc...)
class FileLineReader : public CLIProtocol::LineReader
{
public:
    FileLineReader(std::unique_ptr<std::istream> stream)
    : stream(std::move(stream))
    {
        assert(this->stream);
    }

    virtual std::tuple<string_view, Result> get_line(const char *) override
    {
        std::getline(*stream, line);
        if (!stream->good())
            return {string_view{}, stream->eof() ? Eof : Error};

        return {line, Success};
    }

private:
    std::unique_ptr<std::istream> stream;
    std::string line;
};

// This class reads commands from in memory array...
class MemoryLineReader : public CLIProtocol::LineReader
{
public:
    MemoryLineReader(span<const string_view> commands)
    : commands(commands)
    {}

    virtual std::tuple<string_view, Result> get_line(const char *) override
    {
        if (commands.empty())
            return {string_view{}, Eof};

        auto line = commands.front();
        commands = commands.subspan(1);
        return {line, Success};
    }

private:
    span<const string_view> commands;
};


CLIProtocol::TermSettings::TermSettings()
{
#ifdef WIN32
    auto in = GetStdHandle(STD_INPUT_HANDLE);
    auto out = GetStdHandle(STD_OUTPUT_HANDLE);

    std::pair<DWORD, DWORD> modes;

    if (!GetConsoleMode(in, &modes.first))
        return;

    if (!GetConsoleMode(out, &modes.second))
        return;

    data.reset(reinterpret_cast<char*>(new decltype(modes) {modes}));

    SetConsoleMode(in, modes.first & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    SetConsoleMode(out, modes.second | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

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
    auto modes = reinterpret_cast<std::pair<DWORD, DWORD>*>(data.get());
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), modes->first);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), modes->second);

#else
    tcsetattr(fileno(stdin), TCSADRAIN, reinterpret_cast<termios*>(data.get()));
#endif
}


CLIProtocol::CLIProtocol(InStream& input, OutStream& output) :
  IProtocol(input, output),
  m_input(input),
  m_output(output),
  m_processStatus(NotStarted),
  m_varCounter(0),
  line_reader()
{
    (void)m_input, (void)m_output;  // TODO start usint std::iostream in future
}


int CLIProtocol::printf_checked(const char *fmt, ...)
{
    int len;
    va_list args;

    {
        char buf[2*LINE_MAX];
        va_start(args, fmt);
        len = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (len < 0)
            return -1;

        if (size_t(len) < sizeof(buf))
        {
            cout << buf;
            cout.flush();
            return len;
        }
    }

    std::unique_ptr<char> dbuf(static_cast<char*>(operator new(len + 1)));

    va_start(args, fmt);
    int len2 = vsnprintf(dbuf.get(), len + 1, fmt, args);
    va_end(args);
    if (len < 0 || len > len2)
        return -1;

    cout << dbuf.get();
    cout.flush();
    return len;
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

void CLIProtocol::EmitOutputEvent(OutputCategory category, string_view output, string_view source)
{
    (void)source, (void)category;  // TODO What we should do with category and source?
    
    cout << output;
    cout.flush();
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Backtrace>(const std::vector<std::string> &args_orig, std::string &output)
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

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Break>(const std::vector<std::string> &unmutable_args, std::string &output)
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

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Continue>(const std::vector<std::string> &, std::string &output)
{
    HRESULT Status;
    IfFailRet(m_debugger->Continue(ThreadId::AllThreads));
    output = "^running";
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Delete>(const std::vector<std::string> &args, std::string &)
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

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Detach>(const std::vector<std::string> &args, std::string &)
{
    m_debugger->Disconnect();
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::File>(const std::vector<std::string> &args, std::string &output)
{
    if (args.empty())
    {
        output = "Invalid file name";
        return E_INVALIDARG;
    }
    m_fileExec = args.at(0);
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Finish>(const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, Debugger::STEP_OUT);    
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Help>(const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::commands_list, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoThreads>(const std::vector<std::string> &args, std::string &output)
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


template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoBreakpoints>(const std::vector<std::string>& args, std::string& output)
{
    const static string_view header[] {"#", "Enb", "Rslvd", "Hits", "Source/Function"};
    const static string_view data[] { "99999", "Y", "N", "999999999", "" };
    const static int justify[] = {+1, +1, -1, -1, -1};
    const static char gap[] = "  ";

    static_assert(Utility::Size(header) == Utility::Size(data)
                    && Utility::Size(justify) == Utility::Size(header), "logic error");

    // compute width for each column (excluding gaps between columns)
    static const std::array<unsigned, Utility::Size(header)> widths = []{
            std::array<unsigned, Utility::Size(header)> result {};
            for (unsigned n = 0; n < result.size(); ++n)
                result[n] = unsigned(std::max(header[n].size(), data[n].size()));
            return result;
        }();

    // dashed line length (after the header)
    unsigned static const dashlen = std::accumulate(widths.begin(), widths.end(), 0)
                    + unsigned(Utility::Size(gap)-1) * unsigned(Utility::Size(header) - 1);

    // offset, number of spaces for module name and condition
    unsigned const static offset = dashlen - widths[Utility::Size(widths)-1]
                                    + unsigned(Utility::Size(gap)) - 1;
   
    // prepare dashed line for the header
    char *dashline = static_cast<char*>(alloca(dashlen + 1));
    memset(dashline, '-', dashlen), dashline[dashlen] = 0;

    unsigned nlines = 0;

    // function which prints each particular breakpoint
    auto printer = [&](const Debugger::BreakpointInfo& bp) -> bool
    {
        // print header each few lines
        if (nlines % 24 == 0)
        {
            for (unsigned n = 0; n < Utility::Size(header); n++)
            {
                printf("%s%*.*s",
                    n == 0 ? "" : gap,
                    widths[n]*justify[n], int(header[n].size()), header[n].data());
            }

            printf("\n%.*s\n", dashlen, dashline);
        }

        nlines++;

        // common information for each breakpoint
        printf("%*u%s%*s%s%*s%s%*u%s%.*s",
            widths[0]*justify[0], bp.id, gap,
            widths[1]*justify[1], (bp.enabled ? "y" : "n"), gap,
            widths[2]*justify[2], (bp.resolved ? "y" : "n"), gap,
            widths[3]*justify[3], bp.hit_count, gap,
            int(bp.name.size()), bp.name.data());

        if (!bp.funcsig.empty())
        {
            printf("%.*s", int(bp.funcsig.size()), bp.funcsig.data());
        }
        else if (bp.line)
        {
            printf(":%u", bp.line);
        }

        if (!bp.module.empty())
            printf("\n%*s[in %.*s]", offset, "", int(bp.module.size()), bp.module.data());

        if (!bp.condition.empty())
            printf("\n%*sif (%.*s)", offset, "", int(bp.condition.size()), bp.condition.data());

        printf("\n");

        return true;  // return false to stop enumerating breakpoints
    };

    m_debugger->EnumerateBreakpoints(printer);

    if (nlines == 0)
        output = "No breakpoints.";

    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Interrupt>(const std::vector<std::string> &, std::string &output)
{
    HRESULT Status;
    IfFailRet(m_debugger->Pause());
    output = "^done";
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Next>(const std::vector<std::string> &args, std::string &output)
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


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Print>(const std::vector<std::string> &args, std::string &output)
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

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Quit>(const std::vector<std::string> &, std::string &)
{
    this->m_exit = true;
    m_debugger->Disconnect(Debugger::DisconnectTerminate);
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Run>(const std::vector<std::string> &args, std::string &output)
{
        HRESULT Status;
        m_debugger->Initialize();
        IfFailRet(m_debugger->Launch(m_fileExec, m_execArgs, {}, "", true));
        Status = m_debugger->ConfigurationDone();
        if (SUCCEEDED(Status))
            output = "^running";
        return Status;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Step>(const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, Debugger::STEP_IN);    
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Source>(const std::vector<std::string> &args, std::string &output)
{
    // check arguments
    if (args.empty())
    {
        output = "Argument required (file name).";
        return E_INVALIDARG;
    }

    // open the file
    std::unique_ptr<std::istream> file {new std::ifstream(args[0].c_str())};
    if (file->fail())
    {
        output = args[0] + ": " + ::strerror(errno);
        return E_FAIL;
    }

    return execCommands(FileLineReader(std::move(file)));
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Set>(const std::vector<std::string> &args, std::string &output)
{
    printf("Argument(s) required: see 'help set' for details.\n");
    return S_FALSE;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Info>(const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::info_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoHelp>(const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::info_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetArgs>(const std::vector<std::string> &args, std::string &output)
{
    m_execArgs = args;
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetHelp>(const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::set_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::HelpInfo>(const std::vector<std::string> &args, std::string &output)
{
    return doCommand<CommandTag::InfoHelp>(args, output);
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::HelpSet>(const std::vector<std::string> &args, std::string &output)
{
    return doCommand<CommandTag::SetHelp>(args, output);
}


// This function tries to complete command `str`, where the cursor position is `cursor`:
// functor `func` will be called for each possible completion variant.
// Function should return position starting from which completions might replace the
// text (until cursor position).
unsigned CLIProtocol::completeInput(string_view str, unsigned cursor, const std::function<void(const char*)>& func)
{
    assert(cursor <= str.size());
    return CommandsList::cli_helper.complete(str, cursor,
        [&](void (CLIProtocol::*ptr)(string_view, const std::function<void(const char*)>&), string_view str)
        {
            (this->*ptr)(str, func);
        }
    );
}


// Function handles completion of command names.
template <>
void CLIProtocol::completion_handler<CompletionTag::Command>(string_view command, const std::function<void(const char *)>& func)
{
    // prepare result as C-string.
    char c_str[LINE_MAX];
    assert(command.size() < sizeof(c_str));
    command.copy(c_str, command.size());
    c_str[command.size()] = 0;
    func(c_str);
}



// Function handles completion of arguments for command "break" (filenames or function names).
template <>
void CLIProtocol::completion_handler<CompletionTag::Break>(string_view prefix, const std::function<void(const char *)>& consume)
{
    // Maximum number of variants.
    const static unsigned QueryLimit = 30;

    unsigned count;
    auto counter = [&](const char *) { return ++count; };

    // First just count number of possible completions, and bail out if there is too many options.
    count = 0;
    m_debugger->FindFunctions(prefix, QueryLimit, counter);
    m_debugger->FindFileNames(prefix, QueryLimit - count, counter);
    if (count >= QueryLimit)
    {
        LOGW("too much completions");
        return;
    }

    // Provide completion variants to liblinenoise.
    m_debugger->FindFunctions(prefix, QueryLimit, consume);
    m_debugger->FindFileNames(prefix, QueryLimit, consume);
}


// Function handles completion of arguments for command "print" (variable names).
template <>
void CLIProtocol::completion_handler<CompletionTag::Print>(string_view prefix, const std::function<void(const char *)>& consume)
{
    // Maximum number of variants.
    const static unsigned QueryLimit = 30;

    auto thread = m_debugger->GetLastStoppedThreadId();
    auto frame = FrameLevel{0};

    unsigned count;
    auto counter = [&](const char *) { return ++count, true; };

    // Count of variants and bail out if there is too many variants.
    count = 0;
    m_debugger->FindVariables(thread, frame, prefix, QueryLimit, counter);
    if (count >= QueryLimit)
    {
        LOGW("too much completions");
        return;
    }

    // Provide completions to liblinenoise.
    m_debugger->FindVariables(thread, frame, prefix, QueryLimit, consume);
}


// This function should provide completions for command "delete".
template <>
void CLIProtocol::completion_handler<CompletionTag::Delete>(string_view prefix, const std::function<void(const char *)>& func)
{
    static const string_view words[] {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"};
    CLIHelperBase::complete_words(prefix, func, words);
}


// This function should provide completions for command "file".
template <>
void CLIProtocol::completion_handler<CompletionTag::File>(string_view prefix, const std::function<void(const char *)>& func)
{
    static const string_view words[] {"file.dll", "commands.txt"};
    CLIHelperBase::complete_words(prefix, func, words);
}


// This function prints help for specified (sub)command list.
template <>
HRESULT CLIProtocol::printHelp<CLIProtocol::CLIParams::CommandInfo>(
    const CLIProtocol::CLIParams::CommandInfo *clist, string_view args)
{
    assert(clist != nullptr);

    // separator for aliases
    static const string_view alias_sep = ", ";

    // get greatest column widths for main command name, aliases, and argument info
    std::tuple<int, int, int> widths {0, 0, 0};
    for (const auto* ci = clist; ci->tag != CommandTag::End; ++ci)
    {
        widths = std::make_tuple(
                std::max(std::get<0>(widths), int(ci->names.begin()->size())),
                std::max(std::get<1>(widths), std::accumulate(std::next(ci->names.begin()), ci->names.end(), 0,
                    [](int width, const string_view& alias)
                    {
                        return int(width + alias.size() + (width && alias.size() ? alias_sep.size() : 0));
                    })),
                std::max(std::get<2>(widths), int(ci->aux.args.size())));
    }

    // row headers
    static const string_view header[3] = {"command", "alias", "args"};

    // take into account row headers widths
    widths = decltype(widths){
        std::max(std::get<0>(widths), int(header[0].size())),
        std::max(std::get<1>(widths), int(header[1].size())),
        std::max(std::get<2>(widths), int(header[2].size()))};

    // number of spaces between rows
    static const int spaces[] = {2, 2, 2};

    // print commands descriptions
    const unsigned HeaderLines = 22;  // header will be repeated each HeaderLines lines
    unsigned nlines = 0;
    for (const auto* ci = clist; ci->tag != CommandTag::End; ++ci)
    {
        if (ci->aux.help.empty()) continue;

        // filter-out unwanted commands
        if (!args.empty())
        {
            if (std::find(ci->names.begin(), ci->names.end(), args) == ci->names.end())
                continue;
        }

        // print header, if needed
        if (nlines % HeaderLines == 0)
        {
            int len = printf("%s" "%-*.*s" "%*s" "%-*.*s" "%*s" "%-*.*s\n",
                nlines == 0 ? "" : "\n",
                std::get<0>(widths), int(header[0].size()), header[0].data(), spaces[0], "",
                std::get<1>(widths), int(header[1].size()), header[1].data(), spaces[1], "",
                std::get<2>(widths), int(header[2].size()), header[2].data());

            // print dashes delimiting header and data
            char *line = static_cast<char*>(alloca(len + 1));
            memset(line, '-', len), line[len] = 0;
            printf("%.*s\n", len, line);
        }
        nlines++;

        // print command name
        printf("%-*.*s" "%*s",
            std::get<0>(widths), int(ci->names.begin()->size()), ci->names.begin()->data(),
            spaces[0], "");

        // print all aliases delimited by `alias_sep`
        int len = 0;
        string_view dlm {""};
        for (auto it = std::next(ci->names.begin()); it->size() && it != ci->names.end(); ++it)
        {
            len += printf("%.*s" "%.*s",
                int(dlm.size()), dlm.data(),
                int(it->size()), it->data());

            dlm = alias_sep;
        }

        // print (optional) arguments for the command
        printf("%*s" "%*s" "%-*.*s" "%*s",
            std::get<1>(widths) - len, "",
            spaces[1], "",
            std::get<2>(widths), int(ci->aux.args.size()), ci->aux.args.data(),
            spaces[2], "");

        // print help message (multiline, all lines aligned to beginning of the last column)
        int pspaces = 0;
        int offset = std::get<0>(widths) + spaces[0] + std::get<1>(widths) + spaces[1] + std::get<2>(widths) + spaces[2];
        string_view msg = ci->aux.help;
        while (msg.size())
        {
            char const *p = static_cast<const char*>(memchr(msg.data(), '\n', msg.size()));
            string_view line {msg.data(), p ? p - msg.data() : msg.size()};

            printf("%*s" "%.*s\n",
                pspaces, "",
                int(line.size()), line.data());

            if (msg.size() <= line.size())
                break;

            msg = {msg.data() + line.size() + 1, msg.size() - line.size() - 1};
            pspaces = offset;
        }
    }

    if (nlines == 0 && !args.empty())
        printf("No such command or topic: %.*s\n", int(args.size()), args.data());

    return S_OK;
}


std::tuple<string_view, CLIProtocol::LineReader::Result> CLIProtocol::getLine(const char *prompt)
{
    assert(line_reader);
    return line_reader->get_line(prompt);
}


HRESULT CLIProtocol::execCommands(LineReader&& lr)
{
    // preserve currently existing line reader and restore it on exit
    auto restorer = [&](LineReader* save) { line_reader = save; };
    std::unique_ptr<LineReader, decltype(restorer)> save = {line_reader, restorer};
    line_reader = &lr;

    // Loop in which we read and execute next command.
    string_view input;
    string_view prefix;
    while (!m_exit)
    {
        LineReader::Result result;
        std::tie(input, result) = getLine(Prompt.c_str());

        if(m_processStatus == Exited)    // TODO move this from here...
        {
            m_debugger->Disconnect();
            m_processStatus = NotStarted;
        }

        if (result == LineReader::Eof)
            break;

        if (result ==  LineReader::Error)
        {
            // io error
            return E_FAIL;
        }

        if (result == LineReader::Interrupt)
        {
            m_debugger->Pause();
            continue;
        }

        bool have_result = false;
        std::string output;
        HRESULT hr;

        auto handler = [&](HandlerFunc func, string_view str, size_t prefix_len)
        {
            std::vector<std::string> args;
            args.reserve(10);
            std::string result;
            Tokenizer tokenizer(str.substr(prefix_len));
            while (tokenizer.Next(result))
               args.push_back(result);

            hr = (this->*func)(args, output);
            have_result = true;
        };

        LOGD("evaluating: '%.*s'", int(input.size()), input.data());
        auto unparsed = CommandsList::cli_helper.eval(input, handler);
        if (!have_result)
        {
            if (unparsed.empty())
                continue;

            output = "Unknown command: '" + std::string(unparsed) + "'";
            hr = E_FAIL;
        }

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

            printf("%.*s%s%s\n", int(prefix.size()), prefix.data(), resultClass, output.c_str());
        }
        else
        {
            if (output.empty())
            {
                printf("%s" "%.*s Error: 0x%08x: %s" "%s\n",
                    tty::red.c_str(),
                    int(prefix.size()), prefix.data(), hr, errormessage(hr),
                    tty::reset.c_str());
            }
            else
            {
                printf("%s" "%.*s %s" "%s\n",
                    tty::red.c_str(),
                    int(prefix.size()), prefix.data(), output.c_str(),
                    tty::reset.c_str());
            }
        }
    }
    return S_OK;
}


void CLIProtocol::Source(span<const string_view> init_commands)
{
    execCommands(MemoryLineReader(init_commands));
}


// callback for linenoise library
static unsigned completion_callback(const char *input, unsigned cursor, linenoiseCompletions *lc, void *context)
{
    LOGD("completion: '%s', cursor=%u", input, cursor);
    unsigned result = static_cast<CLIProtocol*>(context)->completeInput({input}, cursor,
                        [&](const char *str) {
                            LOGD("completion variant '%s'\n", str);
                            linenoiseAddCompletion(lc, str); 
                        });
    LOGD("completion substring: [%u, %u)", result, cursor);
    return result;
};


void CLIProtocol::CommandLoop()
{
#ifndef WIN32
    tid = pthread_self();
#endif
    linenoiseInstallWindowChangeHandler();
    linenoiseHistorySetMaxLen(DefaultHistoryDepth);
    linenoiseHistoryLoad(HistoryFileName);

    linenoiseSetCompletionCallbackEx(completion_callback, this);

    // loop till eof, error, or exit request.
    execCommands(ConsoleLineReader());

    printf("^exit\n");

    m_debugger->Disconnect(Debugger::DisconnectTerminate);

    linenoiseHistorySave(HistoryFileName);
    linenoiseHistoryFree();
}



} // namespace netcoredbg
