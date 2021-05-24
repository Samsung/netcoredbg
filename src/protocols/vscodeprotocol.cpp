// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <iomanip>
#include <sstream>

#include <exception>

// note: order matters, vscodeprotocol.h should be included before winerror.h
#include "protocols/vscodeprotocol.h"
#include "winerror.h"

#include "interfaces/idebugger.h"
#include "utils/streams.h"
#include "utils/torelease.h"
#include "utils/utf.h"
#include "utils/logger.h"
#include "utils/escaped_string.h"

using std::string;
using std::vector;
using std::map;
using std::min;

// for convenience
using json = nlohmann::json;

namespace netcoredbg
{

void to_json(json &j, const Source &s) {
    j = json{{"name", s.name},
             {"path", s.path}};
}

void to_json(json &j, const Breakpoint &b) {
    j = json{
        {"id",       b.id},
        {"line",     b.line},
        {"verified", b.verified},
        {"message",  b.message}};
    if (b.verified) {
        j["endLine"] = b.endLine;
        if (!b.source.IsNull())
            j["source"] = b.source;
    }
}

void to_json(json &j, const StackFrame &f) {
    j = json{
        {"id",        int(f.id)},
        {"name",      f.name},
        {"line",      f.line},
        {"column",    f.column},
        {"endLine",   f.endLine},
        {"endColumn", f.endColumn},
        {"moduleId",  f.moduleId}};
    if (!f.source.IsNull())
        j["source"] = f.source;
}

void to_json(json &j, const Thread &t) {
    j = json{{"id",   int(t.id)},
             {"name", t.name}};
          // {"running", t.running}
}

void to_json(json &j, const Scope &s) {
    j = json{
        {"name",               s.name},
        {"variablesReference", s.variablesReference}};

    if (s.variablesReference > 0)
    {
        j["namedVariables"] = s.namedVariables;
        // j["indexedVariables"] = s.indexedVariables;
    }
}

void to_json(json &j, const Variable &v) {
    j = json{
        {"name",               v.name},
        {"value",              v.value},
        {"type",               v.type},
        {"evaluateName",       v.evaluateName},
        {"variablesReference", v.variablesReference}};

    if (v.variablesReference > 0)
    {
        j["namedVariables"] = v.namedVariables;
        // j["indexedVariables"] = v.indexedVariables;
    }
}

static json getVSCode(const ExceptionDetails &self) {
    json details = json({});

    details["message"] = self.message;
    details["typeName"] = self.typeName;
    details["fullTypeName"] = self.fullTypeName;
    details["evaluateName"] = self.evaluateName;
    details["stackTrace"] = self.stackTrace;
    // vsdbg extention: "formattedDescription", "hresult", "source"
    // Example:
    // "formattedDescription":"**System.DivideByZeroException:** '00000:3'",
    // "hresult":-2147352558,
    // "source" : "ClassLibrary1"

    json arr = json::array();
    if (!self.innerException.empty()) {
        // INFO: Visual Studio Code does not display inner exception,
        // but vsdbg fill all nested InnerExceptions in Response.
        const auto it = self.innerException.begin();
        json inner = getVSCode(*it);
        arr.push_back(inner);
    }
    details["innerException"] = arr;

    return details;
}

void VSCodeProtocol::EmitContinuedEvent(ThreadId threadId)
{
    LogFuncEntry();

    json body;

    if (threadId)
        body["threadId"] = int(threadId);

    body["allThreadsContinued"] = true;
    EmitEvent("continued", body);
}

void VSCodeProtocol::EmitStoppedEvent(const StoppedEvent &event)
{
    LogFuncEntry();

    json body;

    switch(event.reason)
    {
        case StopStep:
            body["reason"] = "step";
            break;
        case StopBreakpoint:
            body["reason"] = "breakpoint";
            break;
        case StopException:
            body["reason"] = "exception";
            break;
        case StopPause:
            body["reason"] = "pause";
            break;
        case StopEntry:
            body["reason"] = "entry";
            break;
    }

    body["description"] = event.description;
    body["text"] = event.text;
    body["threadId"] = int(event.threadId);
    body["allThreadsStopped"] = event.allThreadsStopped;

    // vsdbg shows additional info, but it is not a part of the protocol
    // body["line"] = event.frame.line;
    // body["column"] = event.frame.column;

    // body["source"] = event.frame.source;

    EmitEvent("stopped", body);
}

void VSCodeProtocol::EmitExitedEvent(const ExitedEvent &event)
{
    LogFuncEntry();
    json body;
    body["exitCode"] = event.exitCode;
    EmitEvent("exited", body);
}

void VSCodeProtocol::EmitTerminatedEvent()
{
    LogFuncEntry();
    EmitEvent("terminated", json::object());
}

void VSCodeProtocol::EmitThreadEvent(const ThreadEvent &event)
{
    LogFuncEntry();
    json body;

    switch(event.reason)
    {
        case ThreadStarted:
            body["reason"] = "started";
            break;
        case ThreadExited:
            body["reason"] = "exited";
            break;
    }

    body["threadId"] = int(event.threadId);

    EmitEvent("thread", body);
}

void VSCodeProtocol::EmitModuleEvent(const ModuleEvent &event)
{
    LogFuncEntry();
    json body;

    switch(event.reason)
    {
        case ModuleNew:
            body["reason"] = "new";
            break;
        case ModuleChanged:
            body["reason"] = "changed";
            break;
        case ModuleRemoved:
            body["reason"] = "removed";
            break;
    }

    json &module = body["module"];
    module["id"] = event.module.id;
    module["name"] = event.module.name;
    module["path"] = event.module.path;

    switch(event.module.symbolStatus)
    {
        case SymbolsSkipped:
            module["symbolStatus"] = "Skipped loading symbols.";
            break;
        case SymbolsLoaded:
            module["symbolStatus"] = "Symbols loaded.";
            break;
        case SymbolsNotFound:
            module["symbolStatus"] = "Symbols not found.";
            break;
    }

    EmitEvent("module", body);
}


namespace
{
    // Rules to escape characters in strings, in JSON.
    struct JSON_escape_rules
    {
       static const char forbidden_chars[];
       static const char subst_chars[];
       constexpr static const char escape_char = '\\';
    };

    // Allocate static memory for strings declared above.
    const char JSON_escape_rules::forbidden_chars[] = "\b\f\n\r\t\"\\";
    const char JSON_escape_rules::subst_chars[] = "bfnrt\"\\";

    // This function serializes "OutputEvent" to specified output stream and used for two
    // purposes: to compute output size, and to perform the output directly.
    template <typename T1, typename T2>
    void serialize_output(std::ostream& stream, uint64_t counter, string_view name, T1& text, T2& source)
    {
        stream << "{\"seq\":" << counter 
            << ", \"event\":\"output\",\"type\":\"event\",\"body\":{\"category\":\"" << name
            << "\",\"output\":\"" << text << "\"";

        if (source.size() > 0)
            stream << ",\"source\":\"" << source << "\"";

        stream <<  "}}";

        stream.flush();
    };
}

void VSCodeProtocol::EmitOutputEvent(OutputCategory category, string_view output, string_view source)
{
    LogFuncEntry();

    static const string_view categories[] = {"console", "stdout", "stderr"};

    // determine "category name"
    assert(category == OutputConsole || category == OutputStdOut || category == OutputStdErr);
    const string_view& name = categories[category];

    EscapedString<JSON_escape_rules> escaped_text(output);
    EscapedString<JSON_escape_rules> escaped_source(source);

    std::lock_guard<std::mutex> lock(m_outMutex);

    // compute size of headers without text (text could be huge, no reason parse it for size, that we already know)
    CountingStream count;
    serialize_output(count, m_seqCounter, name, "", escaped_source);

    // compute total size of headers + text
    size_t const total_size = count.size() + escaped_text.size();

    // perform output
    cout << CONTENT_LENGTH << total_size << TWO_CRLF;
    serialize_output(cout, m_seqCounter, name, escaped_text, escaped_source);

    ++m_seqCounter;
}

void VSCodeProtocol::EmitBreakpointEvent(const BreakpointEvent &event)
{
    LogFuncEntry();
    json body;

    switch(event.reason)
    {
        case BreakpointNew:
            body["reason"] = "new";
            break;
        case BreakpointChanged:
            body["reason"] = "changed";
            break;
        case BreakpointRemoved:
            body["reason"] = "removed";
            break;
    }

    body["breakpoint"] = event.breakpoint;

    EmitEvent("breakpoint", body);
}

void VSCodeProtocol::EmitInitializedEvent()
{
    LogFuncEntry();
    EmitEvent("initialized", json::object());
}

void VSCodeProtocol::EmitExecEvent(PID pid, const std::string& argv0)
{
    json body;

    body["name"] = argv0;
    body["systemProcessId"] = PID::ScalarType(pid);
    body["isLocalProcess"] = true;
    body["startMethod"] = "launch";

    EmitEvent("process", body);
}

void VSCodeProtocol::EmitCapabilitiesEvent()
{
    LogFuncEntry();

    json body = json::object();
    json capabilities = json::object();

    AddCapabilitiesTo(capabilities);

    body["capabilities"] = capabilities;

    EmitEvent("capabilities", body);
}

void VSCodeProtocol::Cleanup()
{

}

static string VSCodeSeq(uint64_t id) {
    return string("{\"seq\":" + std::to_string(id) + ",");
}

void VSCodeProtocol::EmitEvent(const std::string &name, const nlohmann::json &body)
{
    std::lock_guard<std::mutex> lock(m_outMutex);
    json response;
    response["type"] = "event";
    response["event"] = name;
    response["body"] = body;
    std::string output = response.dump();
    output = VSCodeSeq(m_seqCounter) + output.substr(1);
    ++m_seqCounter;

    cout << CONTENT_LENGTH << output.size() << TWO_CRLF << output;
    cout.flush();
    Log(LOG_EVENT, output);
}

typedef std::function<HRESULT(
    const json &arguments,
    json &body)> CommandCallback;

void VSCodeProtocol::AddCapabilitiesTo(json &capabilities)
{
    capabilities["supportsConfigurationDoneRequest"] = true;
    capabilities["supportsFunctionBreakpoints"] = true;
    capabilities["supportsConditionalBreakpoints"] = true;
    capabilities["supportTerminateDebuggee"] = true;
    capabilities["supportsExceptionInfoRequest"] = true;
    capabilities["supportsSetVariable"] = true;
}

HRESULT VSCodeProtocol::HandleCommand(const std::string &command, const json &arguments, json &body)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "initialize", [this](const json &arguments, json &body){

        EmitCapabilitiesEvent();

        m_sharedDebugger->Initialize();

        AddCapabilitiesTo(body);

        return S_OK;
    } },
    { "setExceptionBreakpoints", [this](const json &arguments, json &body) {
        vector<string> filters = arguments.value("filters", vector<string>());
        ExceptionBreakMode mode;

        namespace KW = VSCodeExceptionBreakModeKeyWord;

        for (unsigned i = 0; i < filters.size(); i++)
        {
            if (filters[i].compare(KW::ALL) == 0 ||
                filters[i].compare(KW::ALWAYS) == 0) {
                mode.setAll();
            }
            if (filters[i].compare(KW::USERUNHANDLED) == 0 ||
                filters[i].compare(KW::USERUNHANDLED_A) == 0) {
                mode.setUserUnhandled();
            }
            // Nothing to do for "unhandled"
            if (filters[i].compare(KW::NEVER) == 0) {
                mode.resetAll();
            }
        }

        const string globalExceptionBreakpoint = "*";
        uint32_t id;
        m_sharedDebugger->InsertExceptionBreakpoint(mode, globalExceptionBreakpoint, id);

        // TODO:
        // - implement options support. options not supported in
        // current vscode 1.31.1 with C# plugin 1.17.1
        // - use ExceptionBreakpointStorage type for support options feature
        body["supportsExceptionOptions"] = false;

        return S_OK;
    } },
    { "configurationDone", [this](const json &arguments, json &body){
        return m_sharedDebugger->ConfigurationDone();
    } },
    { "exceptionInfo", [this](const json &arguments, json &body) {
        ThreadId threadId{int(arguments.at("threadId"))};
        ExceptionInfoResponse exceptionResponse;
        if (!m_sharedDebugger->GetExceptionInfoResponse(threadId, exceptionResponse))
        {
            body["breakMode"] = exceptionResponse.getVSCodeBreakMode();
            body["exceptionId"] = exceptionResponse.exceptionId;
            body["description"] = exceptionResponse.description;
            body["details"] = getVSCode(exceptionResponse.details);
            // vsdbg extension
            // body["code"] = 0;
            return S_OK;
        }
        return E_FAIL;
    } },
    { "setBreakpoints", [this](const json &arguments, json &body){
        HRESULT Status;

        std::vector<LineBreakpoint> lineBreakpoints;
        for (auto &b : arguments.at("breakpoints"))
            lineBreakpoints.emplace_back(std::string(), b.at("line"), b.value("condition", std::string()));

        std::vector<Breakpoint> breakpoints;
        IfFailRet(m_sharedDebugger->SetLineBreakpoints(arguments.at("source").at("path"), lineBreakpoints, breakpoints));

        body["breakpoints"] = breakpoints;

        return S_OK;
    } },
    {"launch", [this](const json &arguments, json &body) {
        auto cwdIt = arguments.find("cwd");
        const string cwd(cwdIt != arguments.end() ? cwdIt.value().get<string>() : std::string{});
        map<string, string> env;
        try {
            env = arguments.at("env").get<map<string, string> >();
        }
        catch (std::exception &ex) {
            LOGI("exception '%s'", ex.what());
            // If we catch inconsistent state on the interrupted reading
            env.clear();
        }
        if (!m_fileExec.empty()) {
            return m_sharedDebugger->Launch(m_fileExec, m_execArgs, env, cwd, arguments.value("stopAtEntry", false));
        }
        vector<string> args = arguments.value("args", vector<string>());
        args.insert(args.begin(), arguments.at("program").get<std::string>());

        m_sharedDebugger->SetJustMyCode(arguments.value("justMyCode", true)); // MS vsdbg have "justMyCode" enabled by default.
        m_sharedDebugger->SetStepFiltering(arguments.value("enableStepFiltering", true)); // MS vsdbg have "enableStepFiltering" enabled by default.

        return m_sharedDebugger->Launch("dotnet", args, env, cwd, arguments.value("stopAtEntry", false));
    } },
    { "threads", [this](const json &arguments, json &body){
        HRESULT Status;
        std::vector<Thread> threads;
        IfFailRet(m_sharedDebugger->GetThreads(threads));

        body["threads"] = threads;

        return S_OK;
    } },
    { "disconnect", [this](const json &arguments, json &body){
        auto terminateArgIter = arguments.find("terminateDebuggee");
        IDebugger::DisconnectAction action;
        if (terminateArgIter == arguments.end())
            action = IDebugger::DisconnectAction::DisconnectDefault;
        else
            action = terminateArgIter.value().get<bool>() ? IDebugger::DisconnectAction::DisconnectTerminate : IDebugger::DisconnectAction::DisconnectDetach;

        m_sharedDebugger->Disconnect(action);

        m_exit = true;
        return S_OK;
    } },
    { "stackTrace", [this](const json &arguments, json &body){
        HRESULT Status;

        int totalFrames = 0;
        ThreadId threadId{int(arguments.at("threadId"))};

        std::vector<StackFrame> stackFrames;
        IfFailRet(m_sharedDebugger->GetStackTrace(
            threadId,
            FrameLevel{arguments.value("startFrame", 0)},
            unsigned(arguments.value("levels", 0)),
            stackFrames,
            totalFrames
            ));

        body["stackFrames"] = stackFrames;
        body["totalFrames"] = totalFrames;

        return S_OK;
    } },
    { "continue", [this](const json &arguments, json &body){
        body["allThreadsContinued"] = true;

        ThreadId threadId{int(arguments.at("threadId"))};
        body["threadId"] = int(threadId);
        return m_sharedDebugger->Continue(threadId);
    } },
    { "pause", [this](const json &arguments, json &body){
        return m_sharedDebugger->Pause();
    } },
    { "next", [this](const json &arguments, json &body){
        return m_sharedDebugger->StepCommand(ThreadId{int(arguments.at("threadId"))}, IDebugger::StepType::STEP_OVER);
    } },
    { "stepIn", [this](const json &arguments, json &body){
        return m_sharedDebugger->StepCommand(ThreadId{int(arguments.at("threadId"))}, IDebugger::StepType::STEP_IN);
    } },
    { "stepOut", [this](const json &arguments, json &body){
        return m_sharedDebugger->StepCommand(ThreadId{int(arguments.at("threadId"))}, IDebugger::StepType::STEP_OUT);
    } },
    { "scopes", [this](const json &arguments, json &body){
        HRESULT Status;
        std::vector<Scope> scopes;
        FrameId frameId{int(arguments.at("frameId"))};
        IfFailRet(m_sharedDebugger->GetScopes(frameId, scopes));

        body["scopes"] = scopes;

        return S_OK;
    } },
    { "variables", [this](const json &arguments, json &body){
        HRESULT Status;

       std::string filterName = arguments.value("filter", "");
        VariablesFilter filter = VariablesBoth;
        if (filterName == "named")
            filter = VariablesNamed;
        else if (filterName == "indexed")
            filter = VariablesIndexed;

        std::vector<Variable> variables;
        IfFailRet(m_sharedDebugger->GetVariables(
            arguments.at("variablesReference"),
            filter,
            arguments.value("start", 0),
            arguments.value("count", 0),
            variables));

        body["variables"] = variables;

        return S_OK;
    } },
    { "evaluate", [this](const json &arguments, json &body){
        HRESULT Status;
        std::string expression = arguments.at("expression");
        FrameId frameId([&](){
            auto frameIdIter = arguments.find("frameId");
            if (frameIdIter == arguments.end())
            {
                ThreadId threadId = m_sharedDebugger->GetLastStoppedThreadId();
                return FrameId{threadId, FrameLevel{0}};
            }
            else {
                return FrameId{int(frameIdIter.value())};
            }
        }());

        // NOTE
        // VSCode don't support evaluation flags, we can't disable implicit function calls during evaluation.
        // https://github.com/OmniSharp/omnisharp-vscode/issues/3173
        Variable variable;
        std::string output;
        Status = m_sharedDebugger->Evaluate(frameId, expression, variable, output);
        if (FAILED(Status))
        {
            body["message"] = output;
            return Status;
        }

        body["result"] = variable.value;
        body["type"] = variable.type;
        body["variablesReference"] = variable.variablesReference;
        if (variable.variablesReference > 0)
        {
            body["namedVariables"] = variable.namedVariables;
            // indexedVariables
        }
        return S_OK;
    } },
    { "attach", [this](const json &arguments, json &body){
        int processId;

        const json &processIdArg = arguments.at("processId");
        if (processIdArg.is_string())
            processId = std::stoi(processIdArg.get<std::string>());
        else if (processIdArg.is_number())
            processId = processIdArg;
        else
            return E_INVALIDARG;

        return m_sharedDebugger->Attach(processId);
    } },
    { "setVariable", [this](const json &arguments, json &body) {
        HRESULT Status;

        std::string name = arguments.at("name");
        std::string value = arguments.at("value");
        int ref = arguments.at("variablesReference");

        std::string output;
        Status = m_sharedDebugger->SetVariable(name, value, ref, output);
        if (FAILED(Status))
        {
            body["message"] = output;
            return Status;
        }

        body["value"] = output;

        return S_OK;
    } },
    { "setFunctionBreakpoints", [this](const json &arguments, json &body) {
        HRESULT Status = S_OK;

        std::vector<FuncBreakpoint> funcBreakpoints;
        for (auto &b : arguments.at("breakpoints"))
        {
            std::string module("");
            std::string params("");
            std::string name = b.at("name");

            std::size_t i = name.find('!');

            if (i != std::string::npos)
            {
                module = std::string(name, 0, i);
                name.erase(0, i + 1);
            }

            i = name.find('(');
            if (i != std::string::npos)
            {
                std::size_t closeBrace = name.find(')');

                params = std::string(name, i, closeBrace - i + 1);
                name.erase(i, closeBrace);
            }

            funcBreakpoints.emplace_back(module, name, params, b.value("condition", std::string()));
        }

        std::vector<Breakpoint> breakpoints;
        IfFailRet(m_sharedDebugger->SetFuncBreakpoints(funcBreakpoints, breakpoints));

        body["breakpoints"] = breakpoints;

        return Status;
    } }
    };

    auto command_it = commands.find(command);
    if (command_it == commands.end())
    {
        return E_NOTIMPL;
    }

    return command_it->second(arguments, body);
}

const std::string VSCodeProtocol::TWO_CRLF("\r\n\r\n");
const std::string VSCodeProtocol::CONTENT_LENGTH("Content-Length: ");

std::string VSCodeProtocol::ReadData()
{
    // parse header (only content len) until empty line
    long content_len = -1;
    while (true)
    {
        std::string line;
        std::getline(cin, line);
        if (!cin.good())
        {
            if (cin.eof()) LOGI("EOF");
            else LOGE("input stream reading error");
            return {};
        }

        if (!line.empty() && line.back() == '\r')
                line.pop_back();

        if (line.empty())
        {
            if (content_len < 0)
            {
                LOGE("protocol error: no 'Content Length:' field!");
                return {};
            }
            break;         // header and content delimiter
        }

        LOGD("header: '%s'", line.c_str());

        if (line.size() > CONTENT_LENGTH.size()
            && std::equal(CONTENT_LENGTH.begin(), CONTENT_LENGTH.end(), line.begin()))
        {
            if (content_len >= 0)
                LOGW("protocol violation: duplicate '%s'", line.c_str());

            char *p;
            errno = 0;
            content_len = strtoul(&line[CONTENT_LENGTH.size()], &p, 10);
            if (errno == ERANGE || !(*p == 0 || isspace(*p)))
            {
                LOGE("protocol violation: '%s'", line.c_str());
                return {};
            }
        }
    }

    std::string result(content_len, 0);
    if (!cin.read(&result[0], content_len))
    {
        if (cin.eof()) LOGE("Unexpected EOF!");
        else LOGE("input stream reading error");
        return {};
    }

    return result;
}

void VSCodeProtocol::CommandLoop()
{
    while (!m_exit)
    {

        std::string requestText = ReadData();
        if (requestText.empty())
            break;

        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            Log(LOG_COMMAND, requestText);
        }

        struct bad_format : public std::invalid_argument
        {
            bad_format(const char *s) : invalid_argument(s) {}
        };

        json response;
        try {
            json request = json::parse(requestText);

            // Variable `resp' is used to construct response and assign it to `response'
            // variable in single step: `response' variable should always be in
            // consistent state (it must not have state when some fields is assigned and
            // some not assigned due to an exception) because `response' is used below
            // in exception handler.
            json resp;
            resp["type"] = "response";
            resp["request_seq"] = request.at("seq");
            response = resp;

            std::string command = request.at("command");
            resp["command"] = command;
            response = resp;

            if (request["type"] != "request")
                throw bad_format("wrong request type!");

            auto argIter = request.find("arguments");
            json arguments = (argIter == request.end() ? json::object() : argIter.value());

            json body = json::object();
            HRESULT Status = HandleCommand(command, arguments, body);

            if (SUCCEEDED(Status))
            {
                resp["success"] = true;
                resp["body"] = body;
            }
            else
            {
                if (body.find("message") == body.end())
                {
                    std::ostringstream ss;
                    ss << "Failed command '" << command << "' : "
                    << "0x" << std::setw(8) << std::setfill('0') << std::hex << Status;
                    resp["message"] = ss.str();
                }
                else
                    resp["message"] = body["message"];

                resp["success"] = false;
            }
            response = resp;
        }
        catch (nlohmann::detail::exception& ex)
        {
            LOGE("JSON error: %s", ex.what());
            response["type"] = "response";
            response["success"] = false;
            response["message"] = std::string("can't parse: ") + ex.what();
        }
        catch (bad_format& ex)
        {
            LOGE("JSON error: %s", ex.what());
            response["type"] = "response";
            response["success"] = false;
            response["message"] = std::string("can't parse: ") + ex.what();
        }

        std::string output = response.dump();

        std::lock_guard<std::mutex> lock(m_outMutex);

        output = VSCodeSeq(m_seqCounter) + output.substr(1);
        ++m_seqCounter;

        cout << CONTENT_LENGTH << output.size() << TWO_CRLF << output;
        cout.flush();
        Log(LOG_RESPONSE, output);
    }

    if (!m_exit)
        m_sharedDebugger->Disconnect();

}

const std::string VSCodeProtocol::LOG_COMMAND("-> (C) ");
const std::string VSCodeProtocol::LOG_RESPONSE("<- (R) ");
const std::string VSCodeProtocol::LOG_EVENT("<- (E) ");

void VSCodeProtocol::EngineLogging(const std::string &path)
{
    if (path.empty())
    {
        m_engineLogOutput = LogConsole;
    }
    else
    {
        m_engineLogOutput = LogFile;
        m_engineLog.open(path);
    }
}

// Caller must care about m_outMutex.
void VSCodeProtocol::Log(const std::string &prefix, const std::string &text)
{
    switch(m_engineLogOutput)
    {
        case LogNone:
            return;
        case LogFile:
            m_engineLog << prefix << text << std::endl;
            m_engineLog.flush();
            return;
        case LogConsole:
        {
            json response;
            response["type"] = "event";
            response["event"] = "output";
            response["body"] = json{
                {"category", "console"},
                {"output", prefix + text + "\n"}
            };
            std::string output = response.dump();
            output = VSCodeSeq(m_seqCounter) + output.substr(1);
            ++m_seqCounter;
            cout << CONTENT_LENGTH << output.size() << TWO_CRLF << output;
            cout.flush();
            return;
        }
    }
}

string ExceptionInfoResponse::getVSCodeBreakMode() const
{
    namespace KW = VSCodeExceptionBreakModeKeyWord;

    if (breakMode.Never())
        return KW::NEVER;

    if (breakMode.All())
        return KW::ALWAYS;

    if (breakMode.OnlyUnhandled())
        return KW::UNHANDLED;

    // Throw() not supported for VSCode
    //  - description of "always: always breaks".
    // if (breakMode.Throw())

    if (breakMode.UserUnhandled())
        return KW::USERUNHANDLED;

    // Logical Error
    return "undefined";
}

} // namespace netcoredbg
