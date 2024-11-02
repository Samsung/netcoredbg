// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifdef _WIN32
#include <ConsoleApi.h>
#include <ProcessEnv.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#define _isatty(fd) ::isatty(fd)
#define _fileno(file) ::fileno(file)
#endif

#include "interfaces/idebugger.h"
#include "debugger/frames.h"
#include "utils/platform.h"
#include "utils/torelease.h"
#include "protocols/cliprotocol.h"
#include "linenoise.h"
#include "utils/utf.h"
#include "utils/filesystem.h"

#include "utils/limits.h"
#include <cstddef>
#include <sstream>
#include <forward_list>
#include <functional>
#include <algorithm>
#include <numeric>
#include <memory>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <csignal>

#include "utils/string_view.h"
#include "utils/span.h"
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

using Utility::literal;
using CommandTag = CLIProtocol::CommandTag;
using CompletionTag = CLIProtocol::CompletionTag;

// Prompts which displayed when netcoredbg expects next command:
const auto CommandPrompt = tty::bold + tty::green + literal("ncdb>") + tty::reset + literal(" ");

const char HistoryFileName[] = ".netcoredbg_hist";
const size_t DefaultHistoryDepth = 1024;

CLIProtocol* CLIProtocol::g_console_owner = nullptr;
std::mutex CLIProtocol::g_console_mutex;

// Tags for all commands (including compound ones, like "info breakpoints")
// known to the command interpreter.
enum class CLIProtocol::CommandTag
{
    Help,
    Backtrace,
    Break,
    Catch,
    Continue,
    Delete,
    Detach,
    Disable,
    Enable,
    File,
    Finish,
    Frame,
    Interrupt,
    List,
    Next,
    Print,
    Quit,
    Run,
    Attach,
    Step,
    Source,
    Wait,

    // set subcommands
    Set,
    SetArgs,
    SetJustMyCode,
    SetStepFiltering,
    SetHelp,

    // info subcommand
    Info,
    InfoThreads,
    InfoBreakpoints,
    InfoHelp,

    // save subcommand
    Save,
    SaveBreakpoints,
    SaveHelp,

    // help subcommands
    HelpInfo,
    HelpSet,
    HelpSave,

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

// Subcommand for "save" command.
constexpr static const CLIParams::CommandInfo save_commands[] =
{
    {CommandTag::SaveBreakpoints,{}, {}, {{"breakpoints", "break"}}, {{"file"}, "Save breakpoints to the file."}},
    {CommandTag::SaveHelp,       {}, {}, {{"help"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

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

    {CommandTag::SetJustMyCode, {}, {}, {{"just-my-code"}},
            {{"1 or 0"},  "Enable or disable Just My Code feature that automatically\n"
                          "steps over calls to system, framework, and other non-user code."}},

    {CommandTag::SetStepFiltering, {}, {}, {{"step-filtering"}},
            {{"1 or 0"},  "Prevent or allow stepping into properties and operators\n"
                          "in managed code."}},

    {CommandTag::SetHelp, {}, {}, {{"help"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// Subcommands for "help" command.
constexpr static const CLIParams::CommandInfo help_commands[] =
{
    {CommandTag::HelpInfo, {}, {},  {{"info"}}, {{}, {}}},
    {CommandTag::HelpSet,  {}, {},  {{"set"}},  {{}, {}}},
    {CommandTag::HelpSave, {}, {},  {{"save"}}, {{}, {}}},

    // This should be placed at end of command (sub)lists.
    {CommandTag::End, {}, {}, {}, {}}
};

// List of currently implemented commands. Help message will be printed in same order.
constexpr static const CLIParams::CommandInfo commands_list[] =
{
    {CommandTag::Backtrace, {}, {}, {{"backtrace", "bt"}},
        {"[all][--thread TID]", "Print backtrace info."}},

    {CommandTag::Break, {}, {{{1, CompletionTag::Break}}}, {{"break", "b"}},
        {"<loc>", "Set breakpoint at specified location, where the\n"
                  "location might be filename.cs:line or function name.\n"
                  "Optional, module name also could be provided as part\n"
                  "of location: module.dll!filename.cs:line\n"
                  "GDB-like command syntax:\n"
                  "break file_name:line_num\n"
                  "break func_name\n"
                  "break func_name(args)\n"
                  "break assembly.dll!func_name\n"
                  "break ... if condition"}},

    {CommandTag::Catch, {}, {}, {{"catch"}},
        {{}, "Set exception breakpoints."}},

    {CommandTag::Continue, {}, {}, {{"continue", "c"}},
        {{}, "Continue debugging after stop/pause."}},

    {CommandTag::Delete, {}, {{{1, CompletionTag::Delete}}}, {{"delete", "clear"}},
        {"<num>", "Delete breakpoint with specified number."}},

    {CommandTag::Detach, {}, {}, {{"detach"}},
        {{}, "Detach from the debugged process."}},

    {CommandTag::Disable, {}, {}, {{"disable"}},
        {{}, "Disable breakpoint N."}},

    {CommandTag::Enable, {}, {}, {{"enable"}},
        {{}, "Enable breakpoint N."}},

    {CommandTag::File, {}, {{{1, CompletionTag::File}}}, {{"file"}},
        {"<file>", "load executable file to debug."}},

    {CommandTag::Finish, {}, {}, {{"finish"}},
        {{}, "Continue execution till end of the current function."}},

    {CommandTag::Frame, {}, {}, {{"frame", "f"}},
        {{}, "Select & display stack frame."}},

    {CommandTag::Interrupt, {}, {}, {{"interrupt"}},
        {{}, "Interrupt program execution, stop all threads."}},

    {CommandTag::List, {}, {}, {{"list", "l"}},
        {{}, "List source code lines, 5 by default."}},

    {CommandTag::Next, {}, {}, {{"next", "n"}},
        {{}, "Step program, through function calls."}},

    {CommandTag::Print, {}, {{{1, CompletionTag::Print}}}, {{"print", "p"}},
        {"<expr>", "Print variable value or evaluate an expression."}},

    {CommandTag::Quit, {}, {}, {{"quit" , "q"}},
        {{}, "Quit the debugger."}},

    {CommandTag::Run, {}, {}, {{"run", "r"}},
        {{}, "Start debugged program."}},

    {CommandTag::Attach, {}, {}, {{"attach"}},
        {{}, "Attach to the debugged process."}},

    {CommandTag::Detach, {}, {}, {{"detach"}},
        {{}, "Detach from the debugged process."}},

    {CommandTag::Step, {}, {}, {{"step", "s"}},
        {{}, "Step program until a different source line."}},

    {CommandTag::Source, {}, {{{1, CompletionTag::File}}}, {{"source"}},
        {"<file>", "Read commands from a file."}},

    {CommandTag::Wait, {}, {}, {{"wait"}},
        {{}, "Wait until debugee stops (in async. mode)"}},

    {CommandTag::Set, set_commands, {}, {{"set"}},
        {"args...", "Set miscellaneous options (see 'help set')"}},

    {CommandTag::Info, info_commands, {}, {{"info"}},
        {"<topic>", "Show misc. things about the program being debugged."}},

    {CommandTag::Save, save_commands, {}, {{"save"}},
        {"args...", "Save misc. things to the files."}},

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
constexpr const CLIProtocol::CLIParams::CommandInfo CLIProtocol::CommandsList::save_commands[];

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
    std::string m_last_command;
public:
    ConsoleLineReader() : cmdline(nullptr, deleter) {}

    virtual std::tuple<string_view, Result> get_line(const char *prompt) override
    {
        errno = 0;
        cmdline.reset(linenoise(prompt));
        if (!cmdline)
            return {string_view{}, errno == EAGAIN ? Interrupt : Eof};

        size_t len = strlen(cmdline.get());
        if ((!len || strspn(cmdline.get(), " \r\n\t") == len) && !m_last_command.empty())
            return {string_view{m_last_command.c_str()}, Success};
        linenoiseHistoryAdd(cmdline.get());
        return {string_view{cmdline.get()}, Success};
    }

    virtual void setLastCommand(std::string lc) override
    {
        m_last_command = lc;
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
        if (stream->eof())
            return {string_view{}, Eof};
        std::getline(*stream, line);
        if(stream->fail())
            return{string_view{}, Error};
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


// Set of functions used to implement reasonable reaction to Ctrl-Z.
#ifdef _WIN32
class StopSignalHandler {};

#else
// This class implements the logic, which allows user to continue to work in
// console after pressing Ctrl-Z (terminal settings should be restored) and then
// after the user brings the process to foreground, user might continue to work
// with interrupted program (for which, terminal settings must be again restored
// to whose, which must be saved before stopping the program).
class StopSignalHandler
{
public:
    StopSignalHandler()
    {
        // Save original terminal settings (to restore after pressing Ctrl-Z)
        // and set SIGTSTP handler (to catch Ctrl-Z). It's assumed, that 
        // Linenoise is still not reconfigured terminal at this moment.
        orig_ts_valid = tcgetattr(STDIN_FILENO, &orig_ts) == 0;
        set_handler(&orig_handler);
    }

    ~StopSignalHandler()
    {
        // Restore original SIGTSTP handler (typically SIG_DFL) on exit
        // (it's assumed, that Linenoise restored terminal settings in that moment).
        sigaction(SIGTSTP, &orig_handler, NULL);
    }

private:
    static bool orig_ts_valid;              // true if `orig_ts` is valid
    static struct termios orig_ts;          // initial terminal settings
    static struct sigaction orig_handler;   // initial SIGTSTP handler

    // This function sets own SIGTSTP handler and, optionally, returns
    // the handler, which was set originally (if `orig` isn't NULL).
    static void set_handler(struct sigaction *orig)
    {
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sa.sa_handler = signal_handler;
        sigaction(SIGTSTP, &sa, orig);
    }

    static void signal_handler(int)
    {
        // errno might be corrupted by following functions
        int saved_errno = errno;

        // save currently set terminal settings (raw mode)
        struct termios ts;
        bool ts_valid = tcgetattr(STDIN_FILENO, &ts) == 0;

        // restore initially set terminal settings (canonical mode)
        if (orig_ts_valid)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_ts);

        // set original SIGTSTP handler, unmask and raise signal again
        sigaction(SIGTSTP, &orig_handler, NULL);
        raise(SIGTSTP);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTSTP);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        // execution will stop here and continues with SIGCONT after using bring process to foreground

        sigprocmask(SIG_BLOCK, &mask, NULL);  // this is needed?
        set_handler(NULL);  // again set own SIGTSTP handler
        
        // restore previously saved terminal settings
        if (ts_valid)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &ts);

        errno = saved_errno;
    }
}; // StopSignalHandler


bool StopSignalHandler::orig_ts_valid;              // true if `orig_ts` is valid
struct termios StopSignalHandler::orig_ts;          // initial terminal settings
struct sigaction StopSignalHandler::orig_handler;   // initial SIGTSTP handler

#endif  // !_WIN32


CLIProtocol::TermSettings::TermSettings(CLIProtocol& protocol)
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

    // mode for IORedirect::async_input
    modes.first |= ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT;
    SetConsoleMode(in, modes.first);
    SetConsoleMode(out, modes.second | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

#else
    if (!isatty(fileno(stdin)))
        return;

    struct termios ts;
    if (tcgetattr(fileno(stdin), &ts) < 0)
        return;

    data.reset(reinterpret_cast<char*>(new termios {ts}));

    // mode for IORedirect::async_input
    ts.c_lflag |= ICANON | ISIG | IEXTEN | ECHONL | ECHO;
    tcsetattr(fileno(stdin), TCSADRAIN, &ts);
#endif

    std::lock_guard<std::mutex> lock(g_console_mutex);
    CLIProtocol::g_console_owner = &protocol;
}


CLIProtocol::TermSettings::~TermSettings()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    CLIProtocol::g_console_owner = nullptr;

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
  m_sourceLine(0),
  m_listSize(10),
  m_stoppedAt(0),
  m_frameIdx(0),
  m_sources(nullptr),
  m_term_settings(*this), 
  line_reader(),
  m_commandMode(CommandMode::Unset)
{
    (void)m_input, (void)m_output;  // TODO start usint std::iostream in future

    // Handle Ctrl-Z.
    Utility::Singleton<StopSignalHandler>::instance();
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
            lock_guard lock(m_cout_mutex);
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

    lock_guard lock(m_cout_mutex);
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

        if (!b.message.empty())
            ss << ", warning: " << b.message;

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

HRESULT CLIProtocol::PrintExceptionBPs(const std::vector<Breakpoint> &breakpoints, size_t bpCnt, std::string &outStr, const std::string &filter)
{
    if (bpCnt > breakpoints.size())
        return E_FAIL;

    if (bpCnt == 0 || breakpoints.empty())
    {
        outStr = "^done";
        return S_OK;
    }

    std::ostringstream oss;
    size_t bpIdx = breakpoints.size() - bpCnt;
    oss << breakpoints[bpIdx].id << " ";
    for (++bpIdx; bpIdx < breakpoints.size(); ++bpIdx)
        oss << breakpoints[bpIdx].id << " ";

    if (bpCnt > 1)
        outStr = "^done, Catchpoints " + oss.str() + "(" + filter + ")";
    else
        outStr = "^done, Catchpoint " + oss.str() + "(" + filter + ")";

    return S_OK;
}

// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitBreakpointEvent(const BreakpointEvent &event)
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
                                IDebugger::StepType stepType)
{
    unique_lock lock(m_mutex);
    switch (m_processStatus)
    {
        case NotStarted:
        case Exited:
            output = "No process.";
            return E_FAIL;

        case Paused:
        {
            lock.unlock();  // debugger function must not be called with locked mutex
            ThreadId threadId{ ProtocolUtils::GetIntArg(args, "--thread", int(m_sharedDebugger->GetLastStoppedThreadId())) };
            HRESULT Status;
            IfFailRet(m_sharedDebugger->StepCommand(threadId, stepType));
            output = "^running";
            {
                lock_guard lock(m_mutex);
                m_processStatus = Running;
                m_frameIdx = 0;
                m_state_cv.notify_all();
            }
            return Status;
        }

        default:
            output = "Process is not stopped.";
            return E_FAIL;
    }
}

HRESULT CLIProtocol::PrintFrameLocation(const StackFrame &stackFrame, std::string &output)
{
    std::ostringstream ss;

    ss << stackFrame.methodName;
    if (!stackFrame.source.IsNull())
    {
        ss << " at "  << stackFrame.source.path << ":" << stackFrame.line;
    }

    output = ss.str();
    return S_OK;
}

HRESULT CLIProtocol::PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame)
{
    HRESULT Status;
    std::ostringstream ss;

    int totalFrames = 0;
    std::vector<StackFrame> stackFrames;
    
    IfFailRet(m_sharedDebugger->GetStackTrace(threadId, lowFrame, int(highFrame) - int(lowFrame), stackFrames, totalFrames));

    if (stackFrames.size() == 0)
    {
        return E_INVALIDARG;
    }

    int currentFrame = int(lowFrame);

    for (const StackFrame &stackFrame : stackFrames)
    {
        ss << "#" << currentFrame << ": ";
        if (stackFrame.unknownFrameAddr)
        {
            // Note, `AddrToString()` return string with proper amount of symbols for current arch.
            // For now, this is 2 ("0x") + number of symbols that need for print max address for current arch.
            std::string tmpString = ProtocolUtils::AddrToString(stackFrame.addr);
            std::fill(tmpString.begin(), tmpString.end(), '?');
            ss << tmpString;
        }
        else
            ss << ProtocolUtils::AddrToString(stackFrame.addr);

        if (!stackFrame.moduleOrLibName.empty())
            ss << " " << stackFrame.moduleOrLibName << "`";

        std::string frameLocation;
        PrintFrameLocation(stackFrame, frameLocation);
        if (!frameLocation.empty())
            ss << " " << frameLocation;
        ss << "\n";
        currentFrame++;
    }

    output = ss.str();
    return S_OK;
}

void CLIProtocol::Cleanup()
{
    m_breakpointsHandle.Cleanup();
}

// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitStoppedEvent(const StoppedEvent &event)
{
    LogFuncEntry();

    // call repaint() at function exit
    std::unique_ptr<void, std::function<void(void*)> >
        on_exit(this, [&](void *)
        { 
            lock_guard lock(m_mutex);
            repaint();
        });

    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);
    if (!event.frame.moduleOrLibName.empty())
    {
        frameLocation = event.frame.moduleOrLibName + "` " + frameLocation;
    }
    m_sourcePath = event.frame.source.path;
    m_sourceLine = event.frame.line - m_listSize / 2;
    m_stoppedAt = event.frame.line;

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            printf("\nstopped, reason: breakpoint %i hit, thread id: %i, stopped threads: all, times= %u, frame={%s}\n",
                  (unsigned int)event.breakpoint.id, int(event.threadId), (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            break;
        }
        case StopStep:
        {
            printf("\nstopped, reason: end stepping range, thread id: %i, stopped threads: all, frame={%s}\n", int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopException:
        {
            printf("\nstopped, reason: exception received, name: %s, exception: %s, stage: %s, category: %s, thread id: %i, stopped-threads: all, frame={%s}\n",
                event.exception_name.c_str(),
                event.exception_message.empty() ? event.text.c_str() : event.exception_message.c_str(),
                event.exception_stage.c_str(),
                event.exception_category.c_str(),
                int(event.threadId),
                frameLocation.c_str());
            break;
        }
        case StopEntry:
        {
            printf("\nstopped, reason: entry point hit, thread id: %i, stopped threads: all, frame={%s}\n",
                int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopPause:
        {
#ifdef INTEROP_DEBUGGING
            if (!event.signal_name.empty())
            {
                printf("\nstopped, reason: interrupted, signal-name=\"%s\", thread id: %i, stopped threads: all, frame={%s}\n",
                       event.signal_name.c_str(), int(event.threadId), frameLocation.c_str());
            }
            else
#endif INTEROP_DEBUGGING
            {
                printf("\nstopped, reason: interrupted, thread id: %i, stopped threads: all, frame={%s}\n",
                       int(event.threadId), frameLocation.c_str());
            }
            break;
        }
        default:
            break;
    }

    {
      lock_guard lock(m_mutex);

      m_processStatus = Paused;
      m_state_cv.notify_all();
    }
}


// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitExitedEvent(const ExitedEvent &event)
{
    LogFuncEntry();
    lock_guard lock(m_mutex);

    m_processStatus = Exited;
    m_state_cv.notify_all();

    printf("\nstopped, reason: exited, exit-code: %i\n", event.exitCode);
    m_exit = true;

    repaint();
}

void CLIProtocol::EmitTerminatedEvent()
{
    LogFuncEntry();
    lock_guard lock(m_mutex);

    m_processStatus = Exited;
    m_state_cv.notify_all();

    resetConsole();
    m_exit = true;
}

// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitContinuedEvent(ThreadId threadId)
{
    LogFuncEntry();
}

void CLIProtocol::EmitInteropDebuggingErrorEvent(const int error_n)
{
    LogFuncEntry();

    char buf[1024];
    printf("Interop debugging disabled due to initialization fail: %s\n", ErrGetStr(error_n, buf, sizeof(buf)));
}

// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitThreadEvent(const ThreadEvent &event)
{
    LogFuncEntry();

    const char *reasonText = "";
    switch(event.reason)
    {
        case ManagedThreadStarted:
            reasonText = event.interopDebugging ? "managed thread created" : "thread created";
            break;
        case ManagedThreadExited:
            reasonText = event.interopDebugging ? "managed thread exited" : "thread exited";
            break;
        case NativeThreadAttached:
            reasonText = "native thread attached";
            break;
        case NativeThreadStarted:
            reasonText = "native thread created";
            break;
        case NativeThreadExited:
            reasonText = "native thread exited";
            break;
        default:
            return;
    }
    printf("\n%s, id: %i\n", reasonText, int(event.threadId));
}


// This function implements Debugger interface and called from ManagedDebugger, 
// as callback function, in separate thread.
void CLIProtocol::EmitModuleEvent(const ModuleEvent &event)
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
        case ModuleRemoved:
            printf("\nlibrary unloaded: %s\n", event.module.path.c_str());
            break;
        default:
            break;
    }
}


// This function implements Debugger interface and called from ManagedDebugger, 
// (from IORedirect class) as callback function, in separate thread.
void CLIProtocol::EmitOutputEvent(OutputCategory category, string_view output, string_view source, DWORD threadId)
{
    LogFuncEntry();

    (void)source, (void)category;  // TODO What we should do with category and source?

    lock_guard lock(m_cout_mutex);
    cout << output;
    cout.flush();   // not thread safe
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Backtrace>(const std::string &, const std::vector<std::string> &args_orig, std::string &output)
{
    lock_guard lock(m_mutex);

    if (m_processStatus == NotStarted || m_processStatus == Exited)
    {
        output = "No process.";
        return E_FAIL;
    }

    if (m_processStatus != Paused)
    {
        output = "Can't get backtrace for running process.";
        return E_FAIL;
    }

    // assuming call of m_sharedDebugger->GetAnything() with locked mutex not lead to deadlock
    ThreadId tid = m_sharedDebugger->GetLastStoppedThreadId();
    if (tid == ThreadId::AllThreads)
    {
        output ="No stack.";
        return E_FAIL;
    }

    std::vector<std::string> args = args_orig;
    ProtocolUtils::StripArgs(args);
    int lowFrame = 0;
    int highFrame = FrameLevel::MaxFrameLevel;
    ProtocolUtils::GetIndices(args, lowFrame, highFrame);

    // command "bt all"
    if (!args.empty() && args[0] == "all")
    {
        std::vector<Thread> threads;
        if (FAILED(m_sharedDebugger->GetThreads(threads, true)))
        {
            output = "No threads.";
            return E_FAIL;
        }

        std::ostringstream ss;
        int number = 1;

        for (const auto &thread : threads)
        {
            std::string stackTrace;
            if (SUCCEEDED(PrintFrames(thread.id, stackTrace, FrameLevel{lowFrame}, FrameLevel{highFrame})))
            {
                ss << "\nThread " << number << ", id=\"" << int(thread.id)
                << "\", name=\"" << thread.name << "\", state=\""
                << (thread.running ? "running" : "stopped") << "\"";
#ifdef INTEROP_DEBUGGING
                ss << ", type=\"" << (thread.managed ? "managed" : "native") << "\"";
#endif // INTEROP_DEBUGGING
                number++;

                ss << "\n" << stackTrace;
            }
        }

        if (ss.str().empty())
        {
            output = "No stacktraces.";
            return E_FAIL;
        }

        output = ss.str();
        return S_OK;
    }

    // command "bt [--thread TID]"
    ThreadId threadId{ ProtocolUtils::GetIntArg(args_orig, "--thread", int(tid)) };
    return PrintFrames(threadId, output, FrameLevel{lowFrame}, FrameLevel{highFrame});
}

static HRESULT BreakCommandSourceWithLine(std::string &args_string, std::string::size_type nameEnd, std::string::size_type colon, std::string &filename, unsigned int &linenum, std::string &output)
{
    filename = args_string.substr(0, nameEnd);
    args_string.erase(0, colon + 1);

    // remove spaces between ':' and number
    std::string::size_type tmp = args_string.find_first_not_of(' ');
    if (tmp == std::string::npos)
    {
        output = "Unknown breakpoint location format";
        return E_FAIL;
    }
    args_string.erase(0, tmp);

    // get line number
    bool ok = false;
    tmp = args_string.find_first_of(' ');
    if (tmp == std::string::npos)
    {
        linenum = ProtocolUtils::ParseInt(args_string, ok);
        args_string = "";
    }
    else
    {
        linenum = ProtocolUtils::ParseInt(args_string.substr(0, tmp), ok);
        args_string.erase(0, tmp);
    }

    if (!ok)
    {
        output = "Unknown breakpoint location format";
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT BreakCommandSourceWithLine(std::string &args_string, std::string &filename, unsigned int &linenum, std::string &output)
{
    // find end of source
    char quote_symbol = args_string[0];
    args_string.erase(0, 1); // erase first quote symbol
    std::string::size_type nameEnd = args_string.find_first_of(quote_symbol);
    if (nameEnd == std::string::npos)
    {
        output = "Wrong breakpoint specified";
        return E_FAIL;
    }

    // find ':' char
    std::string::size_type colon = args_string.find_first_not_of(' ', nameEnd + 1); // skip second quote symbol
    if (colon == std::string::npos || args_string[colon] != ':')
    {
        output = "Unknown breakpoint location format";
        return E_FAIL;
    }

    return BreakCommandSourceWithLine(args_string, nameEnd, colon, filename, linenum, output);
}

static HRESULT BreakCommandFuncWithParams(std::string &args_string, std::string::size_type firstBrace, std::string &funcname, std::string &params, std::string &output)
{
    funcname = args_string.substr(0, firstBrace);

    std::string::size_type secondBrace = args_string.find_first_of(')', firstBrace);
    if (secondBrace == std::string::npos)
    {
        output = "Wrong breakpoint specified";
        return E_FAIL;
    }

    params = args_string.substr(firstBrace + 1, secondBrace - firstBrace - 1);
    args_string.erase(0, secondBrace + 1);
    return S_OK;
}

static HRESULT BreakCommandParseInput(const std::string &input, std::string &filename, unsigned int &linenum, std::string &funcname, std::string &params, std::string &condition, std::string &output)
{
    HRESULT Status;

    // 1) find first 'b' (could be "b" or "break" only here);
    // 2) find first ' ' (after command we have at least one space for sure);
    // 3) find first not ' ' (skip all spaces before expression);
    // Note, we don't check errors here, since at this point `input` string have "b "/"break " substrings for sure.
    std::string args_string = input.substr(input.find_first_not_of(' ', input.find_first_of(' ', input.find_first_of('b'))));

    if (args_string[0] == '\'' || args_string[0] == '"')
    {
        IfFailRet(BreakCommandSourceWithLine(args_string, filename, linenum, output));
    }
    else
    {
        std::string::size_type firstSpace = args_string.find_first_of(' ');
        std::string::size_type firstColon = args_string.find_first_of(':');
        std::string::size_type firstBrace = args_string.find_first_of('(');

        if (firstSpace == std::string::npos &&
            firstColon == std::string::npos &&
            firstBrace == std::string::npos)
        {
            funcname = std::move(args_string);
            return S_OK;
        }
        else if (firstSpace != std::string::npos &&
                 firstColon != std::string::npos &&
                 firstBrace != std::string::npos)
        {
            if (firstSpace < firstColon && firstSpace < firstBrace)
            {
                if (args_string.find_first_not_of(' ', firstSpace) == firstColon)
                {
                    IfFailRet(BreakCommandSourceWithLine(args_string, firstSpace, firstColon, filename, linenum, output));
                }
                else if (args_string.find_first_not_of(' ', firstSpace) == firstBrace)
                {
                    IfFailRet(BreakCommandFuncWithParams(args_string, firstBrace, funcname, params, output));
                }
                else
                {
                    funcname = args_string.substr(0, firstSpace);
                    args_string.erase(0, firstSpace);
                }
            }
            else if (firstColon < firstSpace && firstColon < firstBrace)
            {
                IfFailRet(BreakCommandSourceWithLine(args_string, firstColon, firstColon, filename, linenum, output));
            }
            else
            {
                assert(firstBrace < firstSpace && firstBrace < firstColon);
                IfFailRet(BreakCommandFuncWithParams(args_string, firstBrace, funcname, params, output));
            }
        }
        else if (firstSpace == std::string::npos &&
                 firstColon != std::string::npos &&
                 firstBrace != std::string::npos)
        {
            output = "Unknown breakpoint location format";
            return E_FAIL;
        }
        else if (firstSpace != std::string::npos &&
                 firstColon == std::string::npos &&
                 firstBrace != std::string::npos)
        {
            if (firstSpace < firstBrace)
            {
                if (args_string.find_first_not_of(' ', firstSpace) == firstBrace)
                {
                    IfFailRet(BreakCommandFuncWithParams(args_string, firstBrace, funcname, params, output));
                }
                else
                {
                    funcname = args_string.substr(0, firstSpace);
                    args_string.erase(0, firstSpace);
                }
            }
            else
            {
                IfFailRet(BreakCommandFuncWithParams(args_string, firstBrace, funcname, params, output));
            }
        }
        else if (firstSpace != std::string::npos &&
                 firstColon != std::string::npos &&
                 firstBrace == std::string::npos)
        {
            if (firstSpace < firstColon)
            {
                if (args_string.find_first_not_of(' ', firstSpace) == firstColon)
                {
                    IfFailRet(BreakCommandSourceWithLine(args_string, firstSpace, firstColon, filename, linenum, output));
                }
                else
                {
                    funcname = args_string.substr(0, firstSpace);
                    args_string.erase(0, firstSpace);
                }
            }
            else
            {
                IfFailRet(BreakCommandSourceWithLine(args_string, firstColon, firstColon, filename, linenum, output));
            }
        }
        else if (firstSpace == std::string::npos &&
                 firstColon == std::string::npos &&
                 firstBrace != std::string::npos)
        {
            IfFailRet(BreakCommandFuncWithParams(args_string, firstBrace, funcname, params, output));
        }
        else if (firstSpace != std::string::npos &&
                 firstColon == std::string::npos &&
                 firstBrace == std::string::npos)
        {
            funcname = args_string.substr(0, firstSpace);
            args_string.erase(0, firstSpace);
        }
        else if (firstSpace == std::string::npos &&
                 firstColon != std::string::npos &&
                 firstBrace == std::string::npos)
        {
            IfFailRet(BreakCommandSourceWithLine(args_string, firstColon, firstColon, filename, linenum, output));
        }
    }

    // get condition
    std::string::size_type tmp = std::string::npos;
    if (!args_string.empty())
        tmp = args_string.find_first_not_of(' ');
    if (tmp != std::string::npos)
    {
        // remove spaces
        args_string.erase(0, tmp);

        // remove "if " part and all spaces before expression
        if (args_string.size() < 3 || args_string.rfind("if ", 0) != 0)
        {
            output = "Unknown breakpoint condition format";
            return E_FAIL;
        }

        tmp = args_string.find_first_not_of(' ', 3);
        if (tmp == std::string::npos)
        {
            output = "Unknown breakpoint condition format";
            return E_FAIL;
        }
        args_string.erase(0, tmp);

        if (!args_string.empty())
            condition = std::move(args_string);
        else
        {
            output = "Unknown breakpoint condition format";
            return E_FAIL;
        }
    }

    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Break>(const std::string &input, const std::vector<std::string> &args, std::string &output)
{
    // GDB-like command syntax
    // https://ftp.gnu.org/old-gnu/Manuals/gdb/html_node/gdb_28.html
    // break file_name:line_num
    // break func_name
    // break func_name(args)
    // break assembly.dll!func_name
    // break ... if condition

    if (args.empty())
    {
        output = "Wrong breakpoint specified";
        return E_FAIL;
    }

    HRESULT Status;
    std::string filename;
    unsigned int linenum = 0;
    std::string funcname;
    std::string params;
    std::string condition;
    IfFailRet(BreakCommandParseInput(input, filename, linenum, funcname, params, condition, output));

    std::string modulename;
    Breakpoint breakpoint;
    if (!filename.empty())
    {
        Status = m_breakpointsHandle.SetLineBreakpoint(m_sharedDebugger, modulename, filename, linenum, condition, breakpoint);
    }
    else
    {
        assert(!funcname.empty());
        // check module name filter inside function name (for example, assembly.dll!Main)
        std::string::size_type module_delimiter = funcname.find_first_of('!');
        if (module_delimiter != std::string::npos)
        {
            modulename = funcname.substr(0, module_delimiter);
            funcname.erase(0, module_delimiter + 1);
        }
        // remove spaces in params
        params.erase(std::remove(params.begin(), params.end(), ' '), params.end());

        Status = m_breakpointsHandle.SetFuncBreakpoint(m_sharedDebugger, modulename, funcname, params, condition, breakpoint);
    }

    if (SUCCEEDED(Status))
        PrintBreakpoint(breakpoint, output);
    else
        output = "Unknown breakpoint location format";

    return Status;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Catch>(const std::string &, const std::vector<std::string> &args, std::string &outStr)
{
    if (args.size() < 2)
    {
        outStr = "Command usage: catch [-mda|-native] <unhandled|user-unhandled|throw|throw+user-unhandled> *|<Exception names>";
        return E_INVALIDARG;
    }

    static std::unordered_map<std::string, ExceptionBreakpointFilter> CLIFilters{
        {"throw",                ExceptionBreakpointFilter::THROW},
        {"user-unhandled",       ExceptionBreakpointFilter::USER_UNHANDLED},
        {"throw+user-unhandled", ExceptionBreakpointFilter::THROW_USER_UNHANDLED},
        {"unhandled",            ExceptionBreakpointFilter::UNHANDLED}};

    size_t i = 0;
    ExceptionCategory category;
    
    if (args.at(i) == "-mda")
    {
        category = ExceptionCategory::MDA;
        ++i;
    }
    else if (args.at(i) == "-native")
    {
        ++i;
    }
    else
        category = ExceptionCategory::CLR;

    auto findFilter = CLIFilters.find(args.at(i));
    if (findFilter == CLIFilters.end())
    {
        outStr = "Catch accepts only: 'throw', 'unhandled', 'user-unhandled' and 'throw+user-unhandled' argument as an exception stage";
        return E_INVALIDARG;
    }
    else
        ++i;

    std::vector<ExceptionBreakpoint> exceptionBreakpoints;
    for (auto it = args.begin() + i; it != args.end(); ++it)
    {
        exceptionBreakpoints.emplace_back(category, findFilter->second);
        // In case of "*" debugger must ignore condition check for this filter.
        if (*it != "*")
            exceptionBreakpoints.back().condition.emplace(*it);
        // Note, no negativeCondition changes, since MI protocol works in another way.
    }

    size_t newBreakPointCnt = exceptionBreakpoints.size();
    if (newBreakPointCnt == 0)
        return E_INVALIDARG;

    HRESULT Status;
    std::vector<Breakpoint> breakpoints;
    // `breakpoints` will return all configured exception breakpoints, not only configured by this command.
    // Note, exceptionBreakpoints data will be invalidated by this call.
    IfFailRet(m_breakpointsHandle.SetExceptionBreakpoints(m_sharedDebugger, exceptionBreakpoints, breakpoints));
    // Print only breakpoints configured by this command (last newBreakPointCnt entries).
    IfFailRet(PrintExceptionBPs(breakpoints, newBreakPointCnt, outStr, findFilter->first));

    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Continue>(const std::string &, const std::vector<std::string> &, std::string &output)
{
    {
      lock_guard lock(m_mutex);

      if (m_processStatus == NotStarted || m_processStatus == Exited)
      {
          output = "No process.";
          return E_FAIL;
      }

      if (m_processStatus != Paused)
      {
          output = "Process is not stopped.";
          return E_FAIL;
      }
    }

    HRESULT Status;
    IfFailRet(m_sharedDebugger->Continue(ThreadId::AllThreads));

    {
      lock_guard lock(m_mutex);

      m_processStatus = Running;
      m_frameIdx = 0;
      m_state_cv.notify_all();
    }

    output = "^running";
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Delete>(const std::string &, const std::vector<std::string> &args, std::string &)
{
    std::unordered_set<uint32_t> ids;
    for (const std::string &idStr : args)
    {
        bool ok;
        int id = ProtocolUtils::ParseInt(idStr, ok);
        if (ok)
            ids.insert(id);
    }
    m_breakpointsHandle.DeleteLineBreakpoints(m_sharedDebugger, ids);
    m_breakpointsHandle.DeleteFuncBreakpoints(m_sharedDebugger, ids);
    m_breakpointsHandle.DeleteExceptionBreakpoints(m_sharedDebugger, ids);
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Detach>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    {
      lock_guard lock(m_mutex);

      if (m_processStatus == NotStarted || m_processStatus == Exited)
      {
          output = "No process to detach.";
          return E_FAIL;
      }
    }

    m_sharedDebugger->Disconnect();
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Disable>(const std::string &, const std::vector<std::string> &args, std::string &)
{
    int count = 0, sub = 0;

    for (auto it=args.begin(); it != args.end(); it++)
    {
        if(count == 0 && sub == 0 && !it->compare("breakpoints"))
        {
            count++;
            sub++;
            continue;
        }
        bool er;
        int i = ProtocolUtils::ParseInt(*it, er);
        if (er)
        {
            if (S_OK != m_sharedDebugger->BreakpointActivate(i, false))
                printf("No breakpoint number %d.\n", i);
        }
        else
        {
            printf("Bad breakpoint number %s.\n", it->c_str());
        }
        count++;
    }
    if (count - sub == 0)
    {
        if (S_OK != m_sharedDebugger->AllBreakpointsActivate(false))
        {
            printf("No breakpoints.\n");
        }
    }
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Enable>(const std::string &, const std::vector<std::string> &args, std::string &)
{
    int count = 0, sub = 0;

    for (auto it=args.begin(); it != args.end(); it++)
    {
        if(count == 0 && sub == 0 && !it->compare("breakpoints"))
        {
            count++;
            sub++;
            continue;
        }
        bool er;
        int i = ProtocolUtils::ParseInt(*it, er);
        if (er) 
        {
            if (S_OK != m_sharedDebugger->BreakpointActivate(i, true))
                printf("No breakpoint number %d.\n", i);
        }
        else
        {
            printf("Bad breakpoint number %s.\n", it->c_str());
        }
        count++;
    }
    if (count - sub == 0)
    {
        if (S_OK != m_sharedDebugger->AllBreakpointsActivate(true))
        {
            printf("No breakpoints.\n");
        }
    }
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::File>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    if (args.empty())
    {
        output = "Invalid file name";
        return E_INVALIDARG;
    }

    lock_guard lock(m_mutex);
    m_fileExec = args.at(0);
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Finish>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, IDebugger::StepType::STEP_OUT);
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Frame>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    lock_guard lock(m_mutex);

    if (m_processStatus == NotStarted || m_processStatus == Exited)
    {
        output = "No process.";
        return E_FAIL;
    }

    if (m_processStatus != Paused)
    {
        output = "Can't select frame for running process.";
        return E_FAIL;
    }

    ThreadId tid = m_sharedDebugger->GetLastStoppedThreadId();
    if (tid == ThreadId::AllThreads)
    {
        output ="No stack.";
        return E_FAIL;
    }

    if (args.size() > 1)
    {
        output = "Invalid number of arguments.";
        return E_INVALIDARG;
    }

    int frameIdx = m_frameIdx;
    if (args.size() > 0)
    {
        bool er;
        frameIdx = ProtocolUtils::ParseInt(args.at(0), er);
        if (!er || frameIdx < 0 || frameIdx >= FrameLevel::MaxFrameLevel)
        {
            output = "Invalid argument.";
            return E_INVALIDARG;
        }
    }

    if(FAILED(PrintFrames(tid, output, FrameLevel{frameIdx}, FrameLevel{frameIdx + 1})))
    {
        output = "Frame not found.";
        return E_FAIL;
    }
    m_frameIdx = frameIdx;
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Help>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::commands_list, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoThreads>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    {
      lock_guard lock(m_mutex);

      if (m_processStatus == NotStarted || m_processStatus == Exited)
      {
          output = "No process.";
          return E_FAIL;
      }
    }

    std::vector<Thread> threads;
    if (FAILED(m_sharedDebugger->GetThreads(threads, true)))
    {
        output = "No threads.";
        return E_FAIL;
    }
    std::ostringstream ss;

    ss << "Threads:";

    int number = 1;
    for (const Thread& thread : threads)
    {
        ss << "\n" << number << ": id=\"" << int(thread.id)
           << "\", name=\"" << thread.name << "\", state=\""
           << (thread.running ? "running" : "stopped") << "\"";
#ifdef INTEROP_DEBUGGING
        ss << ", type=\"" << (thread.managed ? "managed" : "native") << "\"";
#endif // INTEROP_DEBUGGING
        number++;
    }
    output = ss.str();
    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoBreakpoints>(const std::string &, const std::vector<std::string>& args, std::string& output)
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
    auto printer = [&](const IDebugger::BreakpointInfo& bp) -> bool
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

    m_sharedDebugger->EnumerateBreakpoints(printer);

    if (nlines == 0)
        output = "No breakpoints.";

    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Interrupt>(const std::string &, const std::vector<std::string> &, std::string &output)
{
    {
      lock_guard lock(m_mutex);

      if (m_processStatus == NotStarted || m_processStatus == Exited)
      {
          output = "No process.";
          return E_FAIL;
      }

      if (m_processStatus == Paused)
      {
          output = "Process is already stopped.";
          return S_OK;
      }
    }

    HRESULT Status;
    IfFailRet(m_sharedDebugger->Pause(ThreadId::AllThreads, EventFormat::CLI));
    output = "^stopped";
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::List>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    HRESULT status = S_OK;
    int line = m_sourceLine;
    int lines = m_listSize;

    std::string params;
    std::string to_repeat = "l";  // eliminate any params to repeat the last command by pressing <Enter> 
                                  // see exception below for "l -"
    if (!args.empty())
    {
        bool er;
        std::string params;
        for (size_t i = 0; i<args.size(); i++)
            params += args[i];

        size_t pos = params.find(",");
        if (pos == 0) // ex: list ,100 -- m_listSize lines till the 100th line
        {
            int i = ProtocolUtils::ParseInt(params.erase(0,1), er);
            if (er) {
                line = i - lines + 1;
            }
            else
            {
                status = E_FAIL;
            }
        }
        else if (pos == params.length()-1) // ex: list 10, -- m_listSize lines starting from 10th
        {
            int i = ProtocolUtils::ParseInt(params, er);
            if (er) {
                line = i;
            }
            else
            {
                status = E_FAIL;
            }
        }
        else if (pos > 0 && pos < params.length()-1) // ex: list 10,100 -- lines from 10th till 100th
        {
            int i = ProtocolUtils::ParseInt(params, er);
            if (er) {
                line = i;
                i = ProtocolUtils::ParseInt(params.erase(0, pos+1), er);
                if (er) {
                    lines = i - line + 1;
                }
                else
                {
                    status = E_FAIL;
                }
            }
            else
            {
                status = E_FAIL;
            }
        }
        else if (params.front() == '-') // ex: list -  -- m_listSize lines just before last printed
        {
            line -= 2 * m_listSize;
            to_repeat = "l -";          // exception for this case to repeat command by pressing <Enter>
        }
        else if (params.front() == '+') // ex: list +  -- m_listSize lines just after last printed
        {

        } else // ex: list 100  -- m_listSize lines with 100th centered
        {
            int i = ProtocolUtils::ParseInt(args[0], er);
            if (er)
            {
                line = i - m_listSize / 2;
            }
            else
            {
                printf("invalid parameter(s)\n");
                return E_FAIL;
            }
        }
    }

    if (status != S_OK)
    {
        printf("Invalid parameter(s). \n");
        return status;
    }

    if (line < 1) {
        line = 1;
    }

    if (m_sources)
    {
        for (int i = 0; i < lines; i++, line++)
        {
            const char* errMessage = nullptr;
            char* toPrint = m_sources->getLine(m_sourcePath, line, &errMessage);
            if (errMessage)
            {
                printf("Source code file: %s\n%s\n", m_sourcePath.c_str(), errMessage);
            }
            if (toPrint)
            {
                if(line == m_stoppedAt)
                    printf(" > %d\t%s\n", line,  toPrint);
                else
                    printf("   %d\t%s\n", line,  toPrint);
            }
            else
                break; // end of file
        }
        m_sourceLine = line;
    }
    line_reader->setLastCommand(to_repeat);
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Next>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, IDebugger::StepType::STEP_OVER);
}

HRESULT CLIProtocol::PrintVariable(const Variable &v, std::ostringstream &ss, bool expand, bool is_static)
{
    if (v.namedVariables > 0 && expand)
    {
        std::vector<Variable> children;

        if (is_static)
            ss << "" << v.name << ": {";
        else
            ss << v.name << " = " << v.value << ": {";

        m_sharedDebugger->GetVariables(v.variablesReference, VariablesBoth, 0, v.namedVariables, children);
        for (auto &child : children)
        {
            bool stm = (child.name == "Static members") ? true : false;
            PrintVariable (child, ss, stm, stm);
            ss << ", ";
        }
        ss << "\b\b}";
    }
    else
    {
        ss << v.name << " = " << v.value;
    }
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Print>(const std::string &input, const std::vector<std::string> &args, std::string &output)
{
    ThreadId threadId;
    FrameId frameId;
    Variable v(0);
    std::ostringstream ss;

    {
        lock_guard lock(m_mutex);

        if (!args.empty())
        {
            // In order to get expression from input string that contain command and spaces (for example, `   p   "test"`):
            // 1) find first 'p' (could be "p" or "print" only here);
            // 2) find first ' ' (after command we have at least one space for sure);
            // 3) find first not ' ' (skip all spaces before expression);
            // Note, we don't check errors here, since at this point `input` string have "p "/"print " substrings for sure.
            m_lastPrintArg = input.substr(input.find_first_not_of(' ', input.find_first_of(' ', input.find_first_of('p'))));
        }
        else if (m_lastPrintArg.empty())
        {
            puts("The history is empty.");
            return S_OK;
        }

        // call of getter should not fire callback, so we can call it with locked mutex
        threadId = m_sharedDebugger->GetLastStoppedThreadId();
        frameId = FrameId(threadId, FrameLevel{m_frameIdx});
    }

    HRESULT Status;
    IfFailRet(m_sharedDebugger->Evaluate(frameId, m_lastPrintArg, v, output));
    v.name = m_lastPrintArg;
    PrintVariable (v, ss, true, false);
    output = ss.str();
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Quit>(const std::string &, const std::vector<std::string> &, std::string &)
{
    // no mutex locking needed here
    m_exit = true;
    m_sources.reset(nullptr);
    m_sharedDebugger->Disconnect(); // Terminate debuggee process if debugger ran this process and detach in case debugger was attached to it.
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Run>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    unique_lock lock(m_mutex);

    if (m_processStatus != NotStarted && m_processStatus != Exited)
    {
        output = "First you should detach from currently debugged process.";
        return E_FAIL;
    }

    // child process should inherit these setting
    removeInterruptHandler();

    const auto& exec_file = m_fileExec;
    const auto& exec_args = m_execArgs;
    lock.unlock();

    HRESULT Status;
    m_sharedDebugger->Initialize();
    m_sources.reset(new SourceStorage(m_sharedDebugger.get()));
    IfFailRet(m_sharedDebugger->Launch(exec_file, exec_args, {}, "", false));

    lock.lock();
    m_commandMode = CommandMode::Synchronous;
    applyCommandMode();
    lock.unlock();

    Status = m_sharedDebugger->ConfigurationDone();
    if (SUCCEEDED(Status))
    {
        output = "^running";

        lock.lock();
        m_processStatus = Running;
        m_frameIdx = 0;
        m_state_cv.notify_all();

        setupInterruptHandler();
    }
    return Status;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Attach>(const std::string &, const std::vector<std::string>& args, std::string& output)
{
    unique_lock lock(m_mutex);

    if (m_processStatus != NotStarted && m_processStatus != Exited)
    {
        output = "First you should detach from currently debugged process.";
        return E_FAIL;
    }

    lock.unlock();

    if (args.size() < 1)
    {
        output = "Argument required (pid of process to attach).";
        return E_INVALIDARG;
    }

    bool ok;
    int pid = ProtocolUtils::ParseInt(args[0], ok);
    if (!ok) return E_INVALIDARG;

    HRESULT Status;
    m_sharedDebugger->Initialize();
    m_sources.reset(new SourceStorage(m_sharedDebugger.get()));
    IfFailRet(m_sharedDebugger->Attach(pid));

    lock.lock();
    m_commandMode = CommandMode::Asynchronous;
    applyCommandMode();
    lock.unlock();

    IfFailRet(m_sharedDebugger->ConfigurationDone());
    IfFailRet(m_sharedDebugger->Pause(ThreadId::AllThreads, EventFormat::CLI));
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Step>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    return StepCommand(args, output, IDebugger::StepType::STEP_IN);
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Source>(const std::string &, const std::vector<std::string> &args, std::string &output)
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
        output = args[0] + ": ";
        char buf[1024];
        output += ErrGetStr(errno, buf, sizeof(buf));
        return E_FAIL;
    }

    return execCommands(FileLineReader(std::move(file)), true);
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Wait>(const std::string &, const std::vector<std::string> &, std::string &)
{
    lock_guard lock(m_mutex);

    // Wait until debugee isn't stopped.
    do m_state_cv.wait(m_mutex);
    while (m_processStatus == Running);

    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Set>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printf("Argument(s) required: see 'help set' for details.\n");
    return S_FALSE;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::Info>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::info_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::InfoHelp>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::info_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::Save>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printf("Argument(s) required: see 'help save' for details.\n");
    return S_FALSE;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SaveBreakpoints>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    if (args.empty())
    {
        output = "Argument required (file name in which to save).";
        return E_INVALIDARG;
    }

    HRESULT result = S_OK;
    const std::string& filename = args[0];

    std::unique_ptr<FILE, std::function<void(FILE*)> >
        file {nullptr, [](FILE *file){ fclose(file); }};

    auto printer = [&](const IDebugger::BreakpointInfo& bp) -> bool
    {
        if (!file)
        {
            file.reset(fopen(filename.c_str(), "w"));
            if (!file)
            {
                output = filename + ": ";
                char buf[1024];
                output += ErrGetStr(errno, buf, sizeof(buf));
                result = E_FAIL;
                return false;
            }
        }

        fputs("break ", file.get());

        if (!bp.condition.empty())
            fprintf(file.get(), "-c \"%.*s\" ", int(bp.condition.size()), bp.condition.data());

        if (!bp.module.empty())
        {
            fwrite(bp.module.data(), bp.module.size(), 1, file.get());
            fputc('!', file.get());
        }

        fwrite(bp.name.data(), bp.name.size(), 1, file.get());

        // TODO function signature and module name may contains spaces!
        if (!bp.funcsig.empty())
        {
            fwrite(bp.funcsig.data(), bp.funcsig.size(), 1, file.get());
        }
        else if (bp.line)
        {
            fprintf(file.get(), ":%u", bp.line);
        }

        fputc('\n', file.get());
        return true;
    };

    m_sharedDebugger->EnumerateBreakpoints(printer);
    return result;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SaveHelp>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::save_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}


template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetArgs>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    lock_guard lock(m_mutex);
    m_execArgs = args;
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetJustMyCode>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    if (args.empty() || (args[0] != "0" && args[0] != "1"))
        return E_INVALIDARG;

    m_sharedDebugger->SetJustMyCode(args[0] == "1");
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetStepFiltering>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    if (args.empty() || (args[0] != "0" && args[0] != "1"))
        return E_INVALIDARG;

    m_sharedDebugger->SetStepFiltering(args[0] == "1");
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::SetHelp>(const std::string &, const std::vector<std::string> &args, std::string &output)
{
    printHelp(CommandsList::set_commands, args.empty() ? string_view{} : string_view{args[0]});
    return S_OK;
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::HelpInfo>(const std::string &input, const std::vector<std::string> &args, std::string &output)
{
    return doCommand<CommandTag::InfoHelp>(input, args, output);
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::HelpSet>(const std::string &input, const std::vector<std::string> &args, std::string &output)
{
    return doCommand<CommandTag::SetHelp>(input, args, output);
}

template <>
HRESULT CLIProtocol::doCommand<CommandTag::HelpSave>(const std::string &input, const std::vector<std::string> &args, std::string &output)
{
    return doCommand<CommandTag::SaveHelp>(input, args, output);
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
    m_sharedDebugger->FindFunctions(prefix, QueryLimit, counter);
    m_sharedDebugger->FindFileNames(prefix, QueryLimit - count, counter);
    if (count >= QueryLimit)
    {
        LOGW("too much completions");
        return;
    }

    // Provide completion variants to liblinenoise.
    m_sharedDebugger->FindFunctions(prefix, QueryLimit, consume);
    m_sharedDebugger->FindFileNames(prefix, QueryLimit, consume);
}


// Function handles completion of arguments for command "print" (variable names).
template <>
void CLIProtocol::completion_handler<CompletionTag::Print>(string_view prefix, const std::function<void(const char *)>& consume)
{
    // Maximum number of variants.
    const static unsigned QueryLimit = 30;

    auto thread = m_sharedDebugger->GetLastStoppedThreadId();
    m_mutex.lock();
    auto frame = FrameLevel{m_frameIdx};
    m_mutex.unlock();

    unsigned count;
    auto counter = [&](const char *) { return ++count, true; };

    // Count of variants and bail out if there is too many variants.
    count = 0;
    m_sharedDebugger->FindVariables(thread, frame, prefix, QueryLimit, counter);
    if (count >= QueryLimit)
    {
        LOGW("too much completions");
        return;
    }

    // Provide completions to liblinenoise.
    m_sharedDebugger->FindVariables(thread, frame, prefix, QueryLimit, consume);
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

    // avoid interruption from async. events
    lock_guard lock(m_cout_mutex);

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


HRESULT CLIProtocol::execCommands(LineReader&& lr, bool printCommands)
{
    // preserve currently existing line reader and restore it on exit
    auto restorer = [&](LineReader* save) { line_reader = save; };
    std::unique_ptr<LineReader, decltype(restorer)> save = {line_reader, restorer};
    line_reader = &lr;

    // Loop in which we read and execute next command, or pass input to the debugee.
    string_view input;
    string_view prefix; // fixme
    bool process_stdin = true;
    bool exited = false;
    while (!m_exit)
    {
        unique_lock lock(m_mutex);

        // TODO move this out here
        // deactivate debugger on process exit (deferred, can't call this in callback)
        if(!exited && m_processStatus == Exited)
        {
            lock.unlock();

            m_sharedDebugger->Disconnect();

            lock.lock();
            exited = 1;
        }

        // should input be passed to debuggee stdin?
        if (process_stdin && m_commandMode == CommandMode::Synchronous && m_processStatus == Running)
        {
            lock.unlock();

            // blocking here on undefined time (till error, EOF or Ctrl-C).
            auto result = m_sharedDebugger->ProcessStdin(dynamic_cast<InStream&>(m_input));
            switch (result)
            {
            default:
                // IDebugger::AsyncResult::Canceled -- nothing to do
                break;

            case IDebugger::AsyncResult::Eof:
                {
                if (dynamic_cast<FileLineReader*>(line_reader) != nullptr)
                    break;

                static const auto ErrorMsg =
                    tty::bold + tty::brown + literal("EOF") + tty::reset + literal("\n");

                printf("%s\n", ErrorMsg.c_str());
                process_stdin = false;
                break;
                }

            case IDebugger::AsyncResult::Error:
                {
                static const auto ErrorMsg =
                    tty::bold + tty::red + literal("stdin reading error!") + tty::reset + literal("\n");

                printf("%s\n", ErrorMsg.c_str());
                process_stdin = false;
                }
                break;
            }

            continue;
        }

        lock.unlock();

        // get command from user (blocking on undefined time)
        LineReader::Result result;
        std::tie(input, result) = getLine(CommandPrompt.c_str());

        if (result == LineReader::Eof)
            break;

        if (result ==  LineReader::Error)
        {
            // io error
            return E_FAIL;
        }

        if (result == LineReader::Interrupt)
        {
            Pause();
            continue;
        }

        // interpret and execute the command...
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

            hr = (this->*func)(str, args, output);
            have_result = true;
        };

        LOGD("executing: '%.*s'", int(input.size()), input.data());

        if (printCommands) {
            printf("%s\n", input.data());
        }
        line_reader->setLastCommand(input);
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
            if (!output.empty())
                printf("%.*s%s\n", int(prefix.size()), prefix.data(), output.c_str());
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


// Note, caller must lock m_mutex.
void CLIProtocol::repaint()
{
    if (m_repaint_fn)
        m_repaint_fn();
}


// set m_repaint_fn depending on m_commandMode
// Note, caller must lock m_mutex.
void CLIProtocol::applyCommandMode()
{
    if (_isatty(_fileno(stdin)))
    {
        // setup function which is called after Stop/Exit events to redraw screen, etc...
#ifndef WIN32
        m_repaint_fn = std::bind(pthread_kill, pthread_self(), SIGWINCH);
#else
        // The SIGILL and SIGTERM signals are not generated under Windows. They are included for ANSI compatibility.
        // Therefore, we can set signal handlers for these signals by using signal, 
        // and we can also explicitly generate these signals by calling raise
        m_repaint_fn = []{ raise(SIGTERM); };
#endif
    }
    else
    {
        // if input comes from non (pseudo) terminals (pipes, files, sockets, etc...)
        // no special function required (because SIGWINCH might not be handled corretly in this case).
        m_repaint_fn = nullptr;
    }
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
    {
        unique_lock lock(m_mutex);
        if (m_commandMode == CommandMode::Unset)
            m_commandMode = CommandMode::Synchronous;
        applyCommandMode();

        // Use linenoise features only if input comes from (pseudo)terminal.
        if (_isatty(_fileno(stdin)))
        {
            linenoiseInstallWindowChangeHandler();
            linenoiseHistorySetMaxLen(DefaultHistoryDepth);
            linenoiseHistoryLoad(HistoryFileName);

            linenoiseSetCompletionCallbackEx(completion_callback, this);
        }
    }

    // loop till eof, error, or exit request.
    execCommands(ConsoleLineReader());

    printf("^exit\n");

    m_sharedDebugger->Disconnect(); // Terminate debuggee process if debugger ran this process and detach in case debugger was attached to it.

    linenoiseHistorySave(HistoryFileName);
    linenoiseHistoryFree();
    cleanupConsoleInputBuffer();

    // At this point we assume, that no EmitStoppedEvent and
    // no EmitExitEvent can occur anymore.

    unique_lock lock(m_mutex);
    m_repaint_fn = nullptr;
}


void CLIProtocol::SetCommandMode(CommandMode mode)
{
    lock_guard lock(m_mutex);
    if (m_commandMode == CommandMode::Unset)
        m_commandMode = mode;
}

void CLIProtocol::SetRunningState()
{
    lock_guard lock(m_mutex);
    if (m_processStatus == NotStarted)
    {
        setupInterruptHandler();
        m_processStatus = Running;
        m_frameIdx = 0;
    }
}

void CLIProtocol::Pause()
{
    unique_lock lock(m_mutex);
    if (m_processStatus == Running)
    {
        lock.unlock();
        m_sharedDebugger->Pause(ThreadId::AllThreads, EventFormat::CLI);
    }
}

// process Ctrl-C events
void CLIProtocol::interruptHandler()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_console_owner)
        g_console_owner->Pause();
}

void CLIProtocol::removeInterruptHandler()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_console_owner != this)
        return;
    
#ifdef _WIN32
    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(NULL, TRUE);
#else
    signal(SIGINT, SIG_IGN);
#endif
}

void CLIProtocol::setupInterruptHandler()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_console_owner != this)
        return;
    
#ifdef _WIN32
    // start handling Ctrl-C events
    auto event_handler = [](DWORD signal) -> BOOL
    {
        if (signal == CTRL_C_EVENT)
        {
            CLIProtocol::interruptHandler();
            return TRUE;
        }
        return FALSE;
    };

    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(event_handler, TRUE);
#else
    signal(SIGINT, [](int){ CLIProtocol::interruptHandler(); });
#endif
}

void CLIProtocol::resetConsole()
{
#ifdef _WIN32
    INPUT_RECORD rec;
    DWORD count;
    rec.EventType = KEY_EVENT;
    rec.Event.KeyEvent.bKeyDown = 1;
    rec.Event.KeyEvent.dwControlKeyState = 0x8; // CONTROL_KEY_STATE_PRESSED;
    rec.Event.KeyEvent.uChar.AsciiChar = 0xd;   // SYMBOL_CTRL_D;
    rec.Event.KeyEvent.uChar.UnicodeChar = 0xd; // SYMBOL_CTRL_D;
    rec.Event.KeyEvent.wRepeatCount = 1;
    rec.Event.KeyEvent.wVirtualKeyCode = 0xd;   // KEYCODE_CTRL_D;
    rec.Event.KeyEvent.wVirtualScanCode = 0x1c; // SCANCODE_CTRL_D;
    WriteConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &rec, 1, &count);
#else
    freopen(static_cast<char*>(nullptr), "wr", stdin);
#endif
}

void CLIProtocol::cleanupConsoleInputBuffer()
{
// No need to do something for linux
#ifdef _WIN32
    DWORD bytesRead;
    HANDLE fh = GetStdHandle(STD_INPUT_HANDLE);
    if (!fh)
        return;
    while (GetNumberOfConsoleInputEvents(fh, &bytesRead) && bytesRead)
    {
        INPUT_RECORD event;
        if (!ReadConsoleInput(fh, &event, 1, &bytesRead))
            return;
    }
#endif
}

} // namespace netcoredbg
