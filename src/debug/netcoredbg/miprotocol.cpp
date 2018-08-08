// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "frames.h"
#include "platform.h"
#include "torelease.h"
#include "miprotocol.h"

#include <sstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>



using namespace std::placeholders;

typedef std::function<HRESULT(
    const std::vector<std::string> &args,
    std::string &output)> CommandCallback;


static int ParseInt(const std::string &s, bool &ok)
{
    ok = false;
    try
    {
        int result = std::stoi(s);
        ok = true;
        return result;
    }
    catch(std::invalid_argument e)
    {
    }
    catch (std::out_of_range  e)
    {
    }
    return 0;
}

// Remove all --name value
static void StripArgs(std::vector<std::string> &args)
{
    auto it = args.begin();

    while (it != args.end())
    {
        if (it->find("--") == 0 && it + 1 != args.end())
            it = args.erase(args.erase(it));
        else
            ++it;
    }
}

static int GetIntArg(const std::vector<std::string> &args, const std::string name, int defaultValue)
{
    auto it = std::find(args.begin(), args.end(), name);

    if (it == args.end())
        return defaultValue;

    ++it;

    if (it == args.end())
        return defaultValue;

    bool ok;
    int val = ParseInt(*it, ok);
    return ok ? val : defaultValue;
}

static bool GetIndices(const std::vector<std::string> &args, int &index1, int &index2)
{
    if (args.size() < 2)
        return false;

    bool ok;
    int val1 = ParseInt(args.at(args.size() - 2), ok);
    if (!ok)
        return false;
    int val2 = ParseInt(args.at(args.size() - 1), ok);
    if (!ok)
        return false;
    index1 = val1;
    index2 = val2;
    return true;
}

bool ParseBreakpoint(const std::vector<std::string> &args_orig, std::string &filename, unsigned int &linenum)
{
    std::vector<std::string> args = args_orig;
    StripArgs(args);

    if (args.empty())
        return false;

    if (args.at(0) == "-f")
    {
        args.erase(args.begin());
        if (args.empty())
            return false;
    }

    std::size_t i = args.at(0).rfind(':');

    if (i == std::string::npos)
        return false;

    filename = args.at(0).substr(0, i);
    std::string slinenum = args.at(0).substr(i + 1);

    bool ok;
    linenum = ParseInt(slinenum, ok);
    return ok && linenum > 0;
}


HRESULT MIProtocol::PrintBreakpoint(const Breakpoint &b, std::string &output)
{
    HRESULT Status;

    std::ostringstream ss;

    if (b.verified)
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\",func=\"\","
              "file=\"" << MIProtocol::EscapeMIValue(b.source.name) << "\","
              "fullname=\"" << MIProtocol::EscapeMIValue(b.source.path) << "\","
              "line=\"" << b.line << "\"}";
        Status = S_OK;
    }
    else
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
              "warning=\"No executable code of the debugger's target code type is associated with this line.\"}";
        Status = S_FALSE;
    }
    output = ss.str();
    return Status;
}

void MIProtocol::EmitBreakpointEvent(BreakpointEvent event)
{
    switch(event.reason)
    {
        case BreakpointChanged:
        {
            std::string output;
            PrintBreakpoint(event.breakpoint, output);
            MIProtocol::Printf("=breakpoint-modified,%s\n", output.c_str());
            return;
        }
        default:
            break;
    }
}

HRESULT MIProtocol::StepCommand(const std::vector<std::string> &args,
                                std::string &output,
                                Debugger::StepType stepType)
{
    DWORD threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
    m_debugger->StepCommand(threadId, stepType);
    output = "^running";
    return S_OK;
}

static std::string AddrToString(uint64_t addr)
{
    std::ostringstream ss;
    ss << "0x" << std::setw(2 * sizeof(void*)) << std::setfill('0') << std::hex << addr;
    return ss.str();
}

HRESULT MIProtocol::PrintFrameLocation(const StackFrame &stackFrame, std::string &output)
{
    std::ostringstream ss;

    if (!stackFrame.source.IsNull())
    {
        ss << "file=\"" << MIProtocol::EscapeMIValue(stackFrame.source.name) << "\","
           << "fullname=\"" << MIProtocol::EscapeMIValue(stackFrame.source.path) << "\","
           << "line=\"" << stackFrame.line << "\","
           << "col=\"" << stackFrame.column << "\","
           << "end-line=\"" << stackFrame.endLine << "\","
           << "end-col=\"" << stackFrame.endColumn << "\",";
    }

    if (stackFrame.clrAddr.methodToken != 0)
    {
        ss << "clr-addr={module-id=\"{" << stackFrame.moduleId << "}\","
           << "method-token=\"0x"
           << std::setw(8) << std::setfill('0') << std::hex << stackFrame.clrAddr.methodToken << "\","
           << "il-offset=\"" << std::dec << stackFrame.clrAddr.ilOffset
           << "\",native-offset=\"" << stackFrame.clrAddr.nativeOffset << "\"},";
    }

    ss << "func=\"" << stackFrame.name << "\"";
    if (stackFrame.id != 0)
        ss << ",addr=\"" << AddrToString(stackFrame.addr) << "\"";

    output = ss.str();

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT MIProtocol::PrintFrames(int threadId, std::string &output, int lowFrame, int highFrame)
{
    HRESULT Status;
    std::ostringstream ss;

    int totalFrames = 0;
    std::vector<StackFrame> stackFrames;
    IfFailRet(m_debugger->GetStackTrace(threadId, lowFrame, highFrame - lowFrame, stackFrames, totalFrames));

    int currentFrame = lowFrame;

    ss << "stack=[";
    const char *sep = "";

    for (const StackFrame &stackFrame : stackFrames)
    {
        ss << sep;
        sep = ",";

        std::string frameLocation;
        PrintFrameLocation(stackFrame, frameLocation);

        ss << "frame={level=\"" << currentFrame << "\"";
        if (!frameLocation.empty())
            ss << "," << frameLocation;
        ss << "}";
        currentFrame++;
    }

    ss << "]";

    output = ss.str();

    return S_OK;
}

HRESULT MIProtocol::PrintVariables(const std::vector<Variable> &variables, std::string &output)
{
    const bool printValues = true;
    const bool printTypes = false;

    std::ostringstream ss;
    ss << "variables=[";
    const char *sep = "";

    for (const Variable &var : variables)
    {
        ss << sep;
        sep = ",";

        ss << "{name=\"" << EscapeMIValue(var.name) << "\"";
        ss << ",value=\"" << EscapeMIValue(var.value) << "\"";
        ss << "}";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

bool MIProtocol::IsEditable(const std::string &type)
{
    if (type == "int"
        || type == "bool"
        || type == "char"
        || type == "byte"
        || type == "sbyte"
        || type == "short"
        || type == "ushort"
        || type == "uint"
        || type == "long"
        || type == "ulong"
        || type == "decimal"
        || type == "string")
        return true;

    return false;
}

void MIProtocol::PrintVar(const std::string &varobjName, Variable &v, int threadId, int print_values, std::string &output)
{
    std::ostringstream ss;

    std::string editable;
    if (IsEditable(v.type))
        editable = "editable";
    else
        editable = "noneditable";

    ss << "name=\"" << varobjName << "\",";
    if (print_values)
    {
        ss << "value=\"" << MIProtocol::EscapeMIValue(v.value) << "\",";
    }
    ss << "attributes=\"" << editable << "\",";
    ss << "exp=\"" << (v.name.empty() ? v.evaluateName : v.name) << "\",";
    ss << "numchild=\"" << v.namedVariables << "\",";
    ss << "type=\"" << v.type << "\",";
    ss << "thread-id=\"" << threadId << "\"";
    //,has_more="0"}

    output = ss.str();
}

void MIProtocol::PrintNewVar(std::string varobjName, Variable &v, int threadId, int print_values, std::string &output)
{
    if (varobjName.empty() || varobjName == "-")
    {
        varobjName = "var" + std::to_string(m_varCounter++);
    }

    m_vars[varobjName] = v;

    PrintVar(varobjName, v, threadId, print_values, output);
}

HRESULT MIProtocol::CreateVar(int threadId, int level, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    uint64_t frameId = StackFrame(threadId, level, "").id;

    Variable variable;
    IfFailRet(m_debugger->Evaluate(frameId, expression, variable));

    int print_values = 1;
    PrintNewVar(varobjName, variable, threadId, print_values, output);

    return S_OK;
}

HRESULT MIProtocol::DeleteVar(const std::string &varobjName)
{
    return m_vars.erase(varobjName) == 0 ? E_FAIL : S_OK;
}

HRESULT MIProtocol::FindVar(const std::string &varobjName, Variable &variable)
{
    auto it = m_vars.find(varobjName);
    if (it == m_vars.end())
        return E_FAIL;

    variable = it->second;

    return S_OK;
}

void MIProtocol::Cleanup()
{
    m_vars.clear();
    m_varCounter = 0;
    m_breakpoints.clear();
}

void MIProtocol::PrintChildren(std::vector<Variable> &children, int threadId, int print_values, bool has_more, std::string &output)
{
    std::ostringstream ss;
    ss << "numchild=\"" << children.size() << "\"";

    if (children.empty())
    {
        output = ss.str();
        return;
    }
    ss << ",children=[";

    const char *sep = "";
    for (auto &child : children)
    {
        std::string varout;
        PrintNewVar("-", child, threadId, print_values, varout);

        ss << sep;
        sep = ",";
        ss << "child={" << varout << "}";
    }

    ss << "]";
    ss << ",has_more=\"" << (has_more ? 1 : 0) << "\"";
    output = ss.str();
}

HRESULT MIProtocol::ListChildren(int threadId, int level, int childStart, int childEnd, const std::string &varName, int print_values, std::string &output)
{
    HRESULT Status;

    StackFrame stackFrame(threadId, level, "");

    std::vector<Variable> variables;

    auto it = m_vars.find(varName);
    if (it == m_vars.end())
        return E_FAIL;

    uint32_t variablesReference = it->second.variablesReference;

    bool has_more = false;

    if (variablesReference > 0)
    {
        IfFailRet(m_debugger->GetVariables(variablesReference, VariablesNamed, childStart, childEnd - childStart, variables));
        has_more = childEnd < m_debugger->GetNamedVariables(variablesReference);
    }

    PrintChildren(variables, threadId, print_values, has_more, output);

    return S_OK;
}

HRESULT MIProtocol::SetBreakpoint(const std::string &filename, int linenum, Breakpoint &breakpoint)
{
    HRESULT Status;

    auto &breakpointsInSource = m_breakpoints[filename];
    std::vector<int> lines;
    for (auto it : breakpointsInSource)
    {
        lines.push_back(it.second);
    }
    lines.push_back(linenum);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(m_debugger->SetBreakpoints(filename, lines, breakpoints));

    for (const Breakpoint& b : breakpoints)
    {
        if (b.line != linenum)
            continue;

        breakpointsInSource.insert(std::make_pair(b.id, b.line));
        breakpoint = b;
        return S_OK;
    }

    return E_FAIL;
}

void MIProtocol::DeleteBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    for (auto &breakpointsIter : m_breakpoints)
    {
        std::vector<int> remainingLines;
        for (auto it : breakpointsIter.second)
        {
            if (ids.find(it.first) == ids.end())
                remainingLines.push_back(it.second);
        }
        if (remainingLines.size() == breakpointsIter.second.size())
            continue;

        std::string filename = breakpointsIter.first;

        std::vector<Breakpoint> tmpBreakpoints;
        m_debugger->SetBreakpoints(filename, remainingLines, tmpBreakpoints);
    }
}

void MIProtocol::EmitStoppedEvent(StoppedEvent event)
{
    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            MIProtocol::Printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",times=\"%u\",frame={%s}\n",
                event.threadId, (unsigned int)event.breakpoint.id, (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            return;
        }
        case StopStep:
        {
            MIProtocol::Printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            return;
        }
        case StopException:
        {
            std::string category = "clr";
            std::string stage = "unhandled";
            MIProtocol::Printf("*stopped,reason=\"exception-received\",exception-name=\"%s\",exception=\"%s\",exception-stage=\"%s\",exception-category=\"%s\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.text.c_str(),
                MIProtocol::EscapeMIValue(event.description).c_str(),
                stage.c_str(),
                category.c_str(),
                event.threadId,
                frameLocation.c_str());
            return;
        }
        case StopPause:
        {
            // When async break happens, this should be reason="interrupted".
            // But MIEngine in Visual Studio accepts only reason="signal-received",signal-name="SIGINT".
            MIProtocol::Printf("*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            return;
        }
        case StopEntry:
        {
            MIProtocol::Printf("*stopped,reason=\"entry-point-hit\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            return;
        }
        default:
            break;
    }
}

void MIProtocol::EmitExitedEvent(ExitedEvent event)
{
    MIProtocol::Printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", event.exitCode);
}

void MIProtocol::EmitContinuedEvent()
{
}

void MIProtocol::EmitThreadEvent(ThreadEvent event)
{
    const char *reasonText = "";
    switch(event.reason)
    {
        case ThreadStarted:
            reasonText = "thread-created";
            break;
        case ThreadExited:
            reasonText = "thread-exited";
            break;
    }
    MIProtocol::Printf("=%s,id=\"%i\"\n", reasonText, event.threadId);
}

void MIProtocol::EmitModuleEvent(ModuleEvent event)
{
    switch(event.reason)
    {
        case ModuleNew:
        {
            std::ostringstream ss;
            ss << "id=\"{" << event.module.id << "}\","
               << "target-name=\"" << MIProtocol::EscapeMIValue(event.module.path) << "\","
               << "host-name=\"" << MIProtocol::EscapeMIValue(event.module.path) << "\","
               << "symbols-loaded=\"" << (event.module.symbolStatus == SymbolsLoaded) << "\","
               << "base-address=\"0x" << std::hex << event.module.baseAddress << "\","
               << "size=\"" << std::dec << event.module.size << "\"";
            Printf("=library-loaded,%s\n", ss.str().c_str());
            break;
        }
        default:
            break;
    }
}

void MIProtocol::EmitOutputEvent(OutputEvent event)
{
    if (event.source.empty())
        MIProtocol::Printf("=message,text=\"%s\",send-to=\"output-window\"\n",
            MIProtocol::EscapeMIValue(event.output).c_str());
    else
        MIProtocol::Printf("=message,text=\"%s\",send-to=\"output-window\",source=\"%s\"\n",
            MIProtocol::EscapeMIValue(event.output).c_str(),
            MIProtocol::EscapeMIValue(event.source).c_str());
}

HRESULT MIProtocol::HandleCommand(std::string command,
                                  const std::vector<std::string> &args,
                                  std::string &output)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status = S_OK;

        std::vector<Thread> threads;
        IfFailRet(m_debugger->GetThreads(threads));

        std::ostringstream ss;

        ss << "threads=[";

        const char *sep = "";
        for (const Thread& thread : threads)
        {
            ss << sep << "{id=\"" << thread.id
               << "\",name=\"" << MIProtocol::EscapeMIValue(thread.name) << "\",state=\""
               << (thread.running ? "running" : "stopped") << "\"}";
            sep = ",";
        }

        ss << "]";
        output = ss.str();
        return S_OK;
    } },
    { "exec-continue", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_debugger->Continue());
        output = "^running";
        return S_OK;
    } },
    { "exec-interrupt", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_debugger->Pause());
        output = "^done";
        return S_OK;
    } },
    { "break-insert", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        std::string filename;
        unsigned int linenum;
        Breakpoint breakpoint;
        if (ParseBreakpoint(args, filename, linenum)
            && SUCCEEDED(SetBreakpoint(filename, linenum, breakpoint)))
        {
            PrintBreakpoint(breakpoint, output);
            return S_OK;
        }

        output = "Unknown breakpoint location format";
        return E_FAIL;
    } },
    { "break-delete", [this](const std::vector<std::string> &args, std::string &) -> HRESULT {
        std::unordered_set<uint32_t> ids;
        for (const std::string &idStr : args)
        {
            bool ok;
            int id = ParseInt(idStr, ok);
            if (ok)
                ids.insert(id);
        }
        DeleteBreakpoints(ids);
        return S_OK;
    } },
    { "exec-step", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_IN);
    }},
    { "exec-next", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_OVER);
    }},
    { "exec-finish", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_OUT);
    }},
    { "exec-abort", [this](const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_debugger->Disconnect(Debugger::DisconnectTerminate);
        return S_OK;
    }},
    { "target-attach", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        if (args.size() != 1)
        {
            output = "Command requires an argument";
            return E_INVALIDARG;
        }
        bool ok;
        int pid = ParseInt(args.at(0), ok);
        if (!ok) return E_INVALIDARG;

        m_debugger->Initialize();
        IfFailRet(m_debugger->Attach(pid));
        IfFailRet(m_debugger->ConfigurationDone());
        // TODO: print succeessful result
        return S_OK;
    }},
    { "target-detach", [this](const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_debugger->Disconnect(Debugger::DisconnectDetach);
        return S_OK;
    }},
    { "stack-list-frames", [this](const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        std::vector<std::string> args = args_orig;
        DWORD threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
        int lowFrame = 0;
        int highFrame = INT_MAX;
        StripArgs(args);
        GetIndices(args, lowFrame, highFrame);
        return PrintFrames(threadId, output, lowFrame, highFrame);
    }},
    { "stack-list-variables", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        int threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
        StackFrame stackFrame(threadId, GetIntArg(args, "--frame", 0), "");
        std::vector<Scope> scopes;
        std::vector<Variable> variables;
        IfFailRet(m_debugger->GetScopes(stackFrame.id, scopes));
        if (!scopes.empty() && scopes[0].variablesReference != 0)
        {
            IfFailRet(m_debugger->GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables));
        }

        PrintVariables(variables, output);

        return S_OK;
    }},
    { "var-create", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        int threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
        int level = GetIntArg(args, "--frame", 0);

        std::string varName = args.at(0);
        std::string varExpr = args.at(1);
        if (varExpr == "*" && args.size() >= 3)
            varExpr = args.at(2);

        return CreateVar(threadId, level, varName, varExpr, output);
    }},
    { "var-list-children", [this](const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        std::vector<std::string> args = args_orig;

        int print_values = 0;
        if (!args.empty())
        {
            auto first_arg_it = args.begin();
            if (*first_arg_it == "1" || *first_arg_it == "--all-values")
            {
                print_values = 1;
                args.erase(first_arg_it);
            }
            else if (*first_arg_it == "2" || *first_arg_it == "--simple-values")
            {
                print_values = 2;
                args.erase(first_arg_it);
            }
        }

        if (args.empty())
        {
            output = "Command requires an argument";
            return E_FAIL;
        }

        int threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
        int level = GetIntArg(args, "--frame", 0);

        int childStart = 0;
        int childEnd = INT_MAX;
        StripArgs(args);
        GetIndices(args, childStart, childEnd);
        std::string varName = args.at(0);

        return ListChildren(threadId, level, childStart, childEnd, varName, print_values, output);
    }},
    { "var-delete", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 1)
        {
            output = "Command requires at least 1 argument";
            return E_FAIL;
        }
        return DeleteVar(args.at(0));
    }},
    { "gdb-exit", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        this->m_exit = true;

        m_debugger->Disconnect(Debugger::DisconnectTerminate);

        return S_OK;
    }},
    { "file-exec-and-symbols", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        m_fileExec = args.at(0);
        return S_OK;
    }},
    { "exec-arguments", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        m_execArgs = args;
        return S_OK;
    }},
    { "exec-run", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        m_debugger->Initialize();
        IfFailRet(m_debugger->Launch(m_fileExec, m_execArgs, true));
        Status = m_debugger->ConfigurationDone();
        if (SUCCEEDED(Status))
            output = "^running";
        return Status;
    }},
    { "environment-cd", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        return SetWorkDir(args.at(0)) ? S_OK : E_FAIL;
    }},
    { "handshake", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!args.empty() && args.at(0) == "init")
            output = "request=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"";

        return S_OK;
    }},
    { "gdb-set", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() == 2)
        {
            if (args.at(0) == "just-my-code")
            {
                m_debugger->SetJustMyCode(args.at(1) == "1");
            }
        }
        return S_OK;
    }},
    { "interpreter-exec", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return S_OK;
    }},
    { "break-exception-insert", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_FAIL;
        size_t i = 1;
        if (args.at(0) == "--mda")
            i = 2;

        std::ostringstream ss;
        const char *sep = "";
        ss << "bkpt=[";
        for (; i < args.size(); i++)
        {
            Breakpoint b;
            m_debugger->InsertExceptionBreakpoint(args.at(i), b);
            ss << sep;
            sep = ",";
            ss << "{number=\"" << b.id << "\"}";
        }
        ss << "]";
        output = ss.str();

        return S_OK;
    }},
    { "var-show-attributes", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        Variable variable;
        std::string varName = args.at(0);
        std::string editable;

        IfFailRet(FindVar(varName, variable));
        if (IsEditable(variable.type))
            editable = "editable";
        else
            editable = "noneditable";

        output = "status=\"" + editable + "\"";
        return S_OK;
    }},
    { "var-assign", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        std::string varName = args.at(0);
        std::string varExpr = args.at(1);

        if (varExpr.size() >= 2 && varExpr.front() == '"' && varExpr.back() == '"')
            varExpr = varExpr.substr(1, varExpr.size() - 2);

        int threadId = GetIntArg(args, "--thread", m_debugger->GetLastStoppedThreadId());
        int level = GetIntArg(args, "--frame", 0);
        uint64_t frameId = StackFrame(threadId, level, "").id;

        Variable variable;
        IfFailRet(FindVar(varName, variable));

        m_debugger->SetVariableByExpression(frameId, variable.evaluateName, varExpr);

        output = "value=\"" + MIProtocol::EscapeMIValue(varExpr) + "\"";

        return S_OK;
    }},
    };

    auto command_it = commands.find(command);

    if (command_it == commands.end())
    {
        output = "Unknown command: " + command;
        return E_FAIL;
    }

    return command_it->second(args, output);
}

class Tokenizer
{
    std::string m_str;
    std::string m_delimiters;
    size_t m_next;
public:

    Tokenizer(const std::string &str, const std::string &delimiters = " \t\n\r")
        : m_str(str), m_delimiters(delimiters), m_next(0)
    {
        m_str.erase(m_str.find_last_not_of(m_delimiters) + 1);
    }

    bool Next(std::string &token)
    {
        token = "";

        if (m_next >= m_str.size())
            return false;

        enum {
            StateSpace,
            StateToken,
            StateQuotedToken,
            StateEscape
        } state = StateSpace;

        for (; m_next < m_str.size(); m_next++)
        {
            char c = m_str.at(m_next);
            switch(state)
            {
                case StateSpace:
                    if (m_delimiters.find(c) != std::string::npos)
                        continue;
                    if (!token.empty())
                        return true;
                    state = c == '"' ? StateQuotedToken : StateToken;
                    if (state != StateQuotedToken)
                        token +=c;
                    break;
                case StateToken:
                    if (m_delimiters.find(c) != std::string::npos)
                        state = StateSpace;
                    else
                        token += c;
                    break;
                case StateQuotedToken:
                    if (c == '\\')
                        state = StateEscape;
                    else if (c == '"')
                        state = StateSpace;
                    else
                        token += c;
                    break;
                case StateEscape:
                    token += c;
                    state = StateQuotedToken;
                    break;
            }
        }
        return state != StateEscape || token.empty();
    }

    std::string Remain() const
    {
        return m_str.substr(m_next);
    }
};

static bool ParseLine(const std::string &str,
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

    if (result.at(i) != '-')
        return false;

    token = result.substr(0, i);
    cmd = result.substr(i + 1);

    if (cmd == "var-assign")
    {
        tokenizer.Next(result);
        args.push_back(result);
        args.push_back(tokenizer.Remain());
        return true;
    }

    while (tokenizer.Next(result))
        args.push_back(result);

    return true;
}

void MIProtocol::CommandLoop()
{
    std::string token;

    while (!m_exit)
    {
        token.clear();
        std::string input;

        Printf("(gdb)\n");
        std::getline(std::cin, input);
        if (input.empty() && std::cin.eof())
            break;

        std::vector<std::string> args;
        std::string command;
        if (!ParseLine(input, token, command, args))
        {
            Printf("%s^error,msg=\"Failed to parse input\"\n", token.c_str());
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

            Printf("%s%s%s\n", token.c_str(), resultClass, output.c_str());
        }
        else
        {
            const char *sep = output.empty() ? "" : " ";
            Printf("%s^error,msg=\"Error: 0x%08x%s%s\"\n", token.c_str(), hr, sep, output.c_str());
        }
    }

    if (!m_exit)
        m_debugger->Disconnect(Debugger::DisconnectTerminate);

    Printf("%s^exit\n", token.c_str());
}

std::mutex MIProtocol::m_outMutex;

void MIProtocol::Printf(const char *fmt, ...)
{
    std::string strbuffer;
    char buffer[32];

    const char *out = nullptr;

    va_list arg;

    va_start(arg, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, arg);
    va_end(arg);

    if (n < 0)
        return;

    if (n >= sizeof(buffer))
    {
        strbuffer.resize(n);

        va_start(arg, fmt);
        n = vsnprintf(&strbuffer[0], strbuffer.size() + 1, fmt, arg);
        va_end(arg);

        if (n < 0 || n > strbuffer.size())
            return;
        out = strbuffer.c_str();
    }
    else
    {
        buffer[n] = '\0';
        out = buffer;
    }

    std::lock_guard<std::mutex> lock(m_outMutex);
    std::cout << out;
    std::cout.flush();
}

std::string MIProtocol::EscapeMIValue(const std::string &str)
{
    std::string s(str);

    for (std::size_t i = 0; i < s.size(); ++i)
    {
        int count = 0;
        char c = s.at(i);
        switch (c)
        {
            case '\"': count = 1; s.insert(i, count, '\\'); s[i + count] = '\"'; break;
            case '\\': count = 1; s.insert(i, count, '\\'); s[i + count] = '\\'; break;
            case '\0': count = 1; s.insert(i, count, '\\'); s[i + count] = '0'; break;
            case '\a': count = 1; s.insert(i, count, '\\'); s[i + count] = 'a'; break;
            case '\b': count = 1; s.insert(i, count, '\\'); s[i + count] = 'b'; break;
            case '\f': count = 1; s.insert(i, count, '\\'); s[i + count] = 'f'; break;
            case '\n': count = 1; s.insert(i, count, '\\'); s[i + count] = 'n'; break;
            case '\r': count = 1; s.insert(i, count, '\\'); s[i + count] = 'r'; break;
            case '\t': count = 1; s.insert(i, count, '\\'); s[i + count] = 't'; break;
            case '\v': count = 1; s.insert(i, count, '\\'); s[i + count] = 'v'; break;
        }
        i += count;
    }

    return s;
}
