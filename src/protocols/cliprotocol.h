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
#include <memory>

#include "debugger/debugger.h"
#include "iprotocol.h"
#include "string_view.h"
#include "span.h"

namespace netcoredbg
{

using Utility::string_view;
using Utility::span;

class CLIProtocol : public IProtocol
{
    enum ProcessStatus
    {
        NotStarted,
        Running,
        Exited
    } m_processStatus;
    
    std::string m_fileExec;
    std::vector<std::string> m_execArgs;

    unsigned int m_varCounter;
    std::unordered_map<std::string, Variable> m_vars;
    std::unordered_map<std::string, std::unordered_map<uint32_t, SourceBreakpoint> > m_breakpoints;
    std::unordered_map<uint32_t, FunctionBreakpoint> m_funcBreakpoints;

#ifndef WIN32
    pthread_t tid;
#endif

    struct TermSettings
    {
        std::unique_ptr<char> data;

        TermSettings();
        ~TermSettings();
    };
    TermSettings ts;

    static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output);
    
public:
    CLIProtocol();

    void EmitInitializedEvent() override {}
    void EmitExecEvent(PID, const std::string& argv) override {}
    void EmitStoppedEvent(StoppedEvent event) override;
    void EmitExitedEvent(ExitedEvent event) override;
    void EmitTerminatedEvent() override {}
    void EmitContinuedEvent(ThreadId threadId) override;
    void EmitThreadEvent(ThreadEvent event) override;
    void EmitModuleEvent(ModuleEvent event) override;
    void EmitOutputEvent(OutputEvent event) override;
    void EmitBreakpointEvent(BreakpointEvent event) override;
    void Cleanup() override;
    void CommandLoop() override;

    void Source(span<const string_view> init_commands = {});

    void SetLaunchCommand(const std::string &fileExec, const std::vector<std::string> &args)
    {
        m_fileExec = fileExec;
        m_execArgs = args;
    }

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
   
    // This function tries to complete command `str`, where the cursor position is `cursor`:
    // functor `func` will be called for each possible completion variant.
    unsigned completeInput(string_view str, unsigned cursor, const std::function<void(const char*)>& func);

    // This function prints help for specified (sub)command list.
    template <typename CommandList> HRESULT printHelp(const CommandList *, string_view args = {});


    HRESULT StepCommand(const std::vector<std::string> &args,
                        std::string &output,
                        Debugger::StepType stepType);
    HRESULT PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame);
    HRESULT SetBreakpoint(const std::string &filename, int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFunctionBreakpoint(const std::string &module, const std::string &funcname, const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT PrintVariable(ThreadId threadId, FrameId frameId, std::list<std::string>::iterator it, Variable v, std::ostringstream &output, bool expand);
    void DeleteBreakpoints(const std::unordered_set<uint32_t> &ids);
    void DeleteFunctionBreakpoints(const std::unordered_set<uint32_t> &ids);
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

        virtual ~LineReader() {}
    };

private:
    // This function should be used by any others CLIProtocol's functions
    // to read input lines for interpreting (commands, etc...)
    std::tuple<string_view, LineReader::Result> getLine(const char *prompt);

    // This function interprets commands from the input till reaching Eof or Error.
    // Function returns E_FAIL in case of input error.
    HRESULT execCommands(LineReader&&);
   
    // Currently active LineReader class, used by getLine() function.
    LineReader *line_reader;

    std::string m_lastPrintArg;
};

} // namespace netcoredbg
