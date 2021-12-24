// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <memory>
#include <atomic>

#include "interfaces/iprotocol.h"
#include "utils/string_view.h"
#include "utils/streams.h"
#include "utils/span.h"
#include "sourcestorage.h"

namespace netcoredbg
{

using Utility::string_view;
template <typename T> using span = Utility::span<T>;

class CLIProtocol : public IProtocol
{
    std::istream& m_input;
    std::ostream& m_output;

    // All function acessing m_output (calling cout << something or printf) must lock
    // this mutex (because in control loop flush() function used which isn't thread safe).
    std::recursive_mutex m_cout_mutex;

    // This mutex protects all class variables which might be accessed from threads
    // other than command loop. All function which read/modify m_processStatus
    // and other local variables must lock the mutex. 
    std::recursive_mutex m_mutex;

    using lock_guard = std::lock_guard<std::recursive_mutex>;
    using unique_lock = std::unique_lock<std::recursive_mutex>;

    enum ProcessStatus
    {
        NotStarted,
        Running,
        Paused,
        Exited
    } m_processStatus;

    // This signalled every time, when m_processStatus changes it's value.
    std::condition_variable_any m_state_cv;
    
    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    std::unordered_map<std::string, std::unordered_map<uint32_t, LineBreakpoint> > m_lineBreakpoints;
    std::unordered_map<uint32_t, FuncBreakpoint> m_funcBreakpoints;
    std::unordered_map<uint32_t, ExceptionBreakpoint> m_exceptionBreakpoints;

    FrameId m_frameId;
    std::string m_sourcePath;
    std::string m_sourceFile;
    int m_sourceLine;
    int m_listSize;
    int m_stoppedAt;
    std::unique_ptr<SourceStorage> m_sources;

    // Functor which is called when UI repaint required.
    std::function<void()> m_repaint_fn;

    struct TermSettings
    {
        std::unique_ptr<char> data;

        TermSettings(CLIProtocol&);
        ~TermSettings();
    };
    TermSettings m_term_settings;

    int printf_checked(const char *fmt, ...);

    static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output);
    static HRESULT PrintExceptionBPs(const std::vector<Breakpoint> &breakpoints, size_t bpCnt, std::string &outStr, const std::string &filter);
    
public:
    CLIProtocol(InStream& input, OutStream& output);

    void EmitInitializedEvent() override {}
    void EmitExecEvent(PID, const std::string& argv) override {}
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

    void Source(span<const string_view> init_commands = {});

    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args) override
    {
        lock_guard lock(m_mutex);
        m_fileExec = fileExec;
        m_execArgs = args;
    }

    enum class CommandMode
    {
        Asynchronous,
        Synchronous,
        Unset
    };

    // This function might be called only once, before entering command loop.
    // SetCommandMode(Asynchronous) must be called if process is attached before
    // entering the command loop.
    void SetCommandMode(CommandMode mode);

    // Inform the protocol class, that debugee is already exist (attached or started new).
    // This function should be called before entering command loop.
    void SetRunningState();

    // Forward declaration of command tags (each distinct command have unique tag)
    // and completion tags. The tags itself defined in cliprotocol.cpp file.
    enum class CommandTag;
    enum class CompletionTag;

private:
    struct CommandsList;

    template <typename T> struct Expose
    {
        using CommandInfo = typename T::CLIParams::CommandInfo;
    };

    using CLIParams = Expose<CommandsList>;

    // Type of the member function pointer, which handles every user command.
    typedef HRESULT (CLIProtocol::*HandlerFunc)(const std::vector<std::string> &args, std::string &output);

    // This function template should be explicitly specialized by command tag
    // to handle each particular user command.
    template <CommandTag> HRESULT doCommand(const std::vector<std::string> &, std::string &);

    // This type maps particular command tag to particular specialization
    // of `doCommand<Tag>` function. This is required to dispatch commands handlers.
    template <CommandTag Tag> struct BindHandler
    {
        static const constexpr CLIProtocol::HandlerFunc handler = &CLIProtocol::doCommand<Tag>;
    };

    // Declaring set of functions, which will hadle completions for each particular `CompletionTag` value.
    template <CompletionTag> void completion_handler(string_view str, const std::function<void (const char*)>& func);

    // This template binds completion `Tag` value with `completion_handler<Tag>` function.
    // This template should be passed to `DispatchTable` template to generate array, which
    // resolves any `Tag` value to corresponding `completion_handler<Tag>` function.
    template <CompletionTag Tag> struct BindCompletions
    {
        constexpr static void(CLIProtocol::*const handler)(string_view, const std::function<void(const char*)>&) = 
            &CLIProtocol::completion_handler<Tag>;
    };

public:
    // This function tries to complete command `str`, where the cursor position is `cursor`:
    // functor `func` will be called for each possible completion variant.
    unsigned completeInput(string_view str, unsigned cursor, const std::function<void(const char*)>& func);

private:
    // This function prints help for specified (sub)command list.
    template <typename CommandList> HRESULT printHelp(const CommandList *, string_view args = {});


    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        IDebugger::StepType stepType);
    HRESULT PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame);
    HRESULT SetLineBreakpoint(const std::string &module, const std::string &filename, int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFuncBreakpoint(const std::string &module, const std::string &funcname, const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT SetExceptionBreakpoints(std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT PrintVariable(const Variable &v, std::ostringstream &output, bool expand, bool is_static);
    void DeleteLineBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteFuncBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteExceptionBreakpoints(const std::unordered_set<uint32_t> &ids);
    static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output);
    bool ParseLine(const std::string &str, std::string &token, std::string &cmd, std::vector<std::string> &args);

public:
    // The interface used to read lines from console or a file.
    class LineReader
    {
    public:
        enum Result
        {
            Eof,        // end of file
            Error,      // can't read file
            Interrupt,  // user request to interrupt debugee (from console)
            Success     // line was read succesfully
        };

        // This function reads next line (LineReader retain ownership
        // of just read line till next call to get_line).
        virtual std::tuple<string_view, Result> get_line(const char *prompt) = 0;
        virtual void setLastCommand(std::string lc) {}

        virtual ~LineReader() {}
    };

private:
    // This function should be used by any others CLIProtocol's functions
    // to read input lines for interpreting (commands, etc...)
    std::tuple<string_view, LineReader::Result> getLine(const char *prompt);

    // This function interprets commands from the input till reaching Eof or Error.
    // Function returns E_FAIL in case of input error.
    HRESULT execCommands(LineReader&&, bool printCommands = false);

    // update screen (after asynchronous message printed)
    void repaint();

    // set m_repaint_fn depending on m_commandMode
    void applyCommandMode();
   
    // Currently active LineReader class, used by getLine() function.
    LineReader *line_reader;

    CommandMode m_commandMode;

    std::string m_lastPrintArg;

    // CLIProtocol instance currently owning console
    static CLIProtocol* g_console_owner;
    static std::mutex g_console_mutex; // mutex which protect g_console_owner

    // pause debugee execution
    void Pause();

    // process Ctrl-C events
    static void interruptHandler();

    // remove/set Ctrl-C handlers
    void removeInterruptHandler();
    void setupInterruptHandler();

    void resetConsole();
    void cleanupConsoleInputBuffer();
};

} // namespace netcoredbg
