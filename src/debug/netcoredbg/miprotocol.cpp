// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "frames.h"
#include "platform.h"
#include "torelease.h"
#include "miprotocol.h"
#include "tokenizer.h"

#include <sstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>

#include "logger.h"

using namespace std::placeholders;
using std::unordered_set;
using std::string;
using std::vector;

typedef std::function<HRESULT(
    const std::vector<std::string> &args,
    std::string &output)> CommandCallback;

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
    LogFuncEntry();

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

HRESULT MIProtocol::CreateVar(int threadId, int level, int evalFlags, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    uint64_t frameId = StackFrame(threadId, level, "").id;

    Variable variable(evalFlags);
    IfFailRet(m_debugger->Evaluate(frameId, expression, variable, output));

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

HRESULT MIProtocol::SetBreakpoint(
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

HRESULT MIProtocol::SetFunctionBreakpoint(
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

HRESULT MIProtocol::SetBreakpointCondition(uint32_t id, const std::string &condition)
{
    // For each file
    for (auto &breakpointsIter : m_breakpoints)
    {
        std::unordered_map<uint32_t, SourceBreakpoint> &fileBreakpoints = breakpointsIter.second;

        // Find breakpoint with specified id in this file
        const auto &sbIter = fileBreakpoints.find(id);
        if (sbIter == fileBreakpoints.end())
            continue;

        // Modify breakpoint condition
        sbIter->second.condition = condition;

        // Gather all breakpoints in this file
        std::vector<SourceBreakpoint> existingBreakpoints;
        existingBreakpoints.reserve(fileBreakpoints.size());
        for (const auto &it : fileBreakpoints)
            existingBreakpoints.emplace_back(it.second);

        // Update breakpoints data for this file
        const std::string &filename = breakpointsIter.first;
        std::vector<Breakpoint> tmpBreakpoints;
        return m_debugger->SetBreakpoints(filename, existingBreakpoints, tmpBreakpoints);
    }

    return E_FAIL;
}

HRESULT MIProtocol::SetFunctionBreakpointCondition(uint32_t id, const std::string &condition)
{
    const auto &fbIter = m_funcBreakpoints.find(id);
    if (fbIter == m_funcBreakpoints.end())
        return E_FAIL;

    fbIter->second.condition = condition;

    std::vector<FunctionBreakpoint> existingFuncBreakpoints;
    existingFuncBreakpoints.reserve(m_funcBreakpoints.size());
    for (const auto &fb : m_funcBreakpoints)
        existingFuncBreakpoints.emplace_back(fb.second);

    std::vector<Breakpoint> tmpBreakpoints;
    return m_debugger->SetFunctionBreakpoints(existingFuncBreakpoints, tmpBreakpoints);
}

void MIProtocol::DeleteBreakpoints(const std::unordered_set<uint32_t> &ids)
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

void MIProtocol::DeleteFunctionBreakpoints(const std::unordered_set<uint32_t> &ids)
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

HRESULT MIProtocol::InsertExceptionBreakpoints(const ExceptionBreakMode &mode,
    const vector<string>& names, string &output)
{
    if (names.empty())
        return E_FAIL;

    HRESULT Status;
    string buf = "";
    uint32_t id = 0;
    for (const auto &name : names) {
        Status = m_debugger->InsertExceptionBreakpoint(mode, name, id);
        if (S_OK != Status) {
            return Status;
        }
        buf += "{number=\"" + std::to_string(id) + "\"},";
    }
    if (!buf.empty())
        buf.pop_back();

    // This line fixes double comma ',,' in output
    if (names.size() > 1) {
        output = "^done,bkpt=[" + buf + "]";
    }
    else {
    // This sensitive for current CI Runner.cs
        output = "^done,bkpt=" + buf;
    }

    return S_OK;
}

HRESULT MIProtocol::DeleteExceptionBreakpoints(const std::unordered_set<uint32_t> &ids,
    string &output)
{
    HRESULT Status;
    for (const auto &id : ids) {
        Status = m_debugger->DeleteExceptionBreakpoint(id);
        if (S_OK != Status) {
            output = "Cannot delete exception breakpoint by id=:'" + std::to_string(id) + "'";
            return Status;
        }
    }
    return S_OK;
}

void MIProtocol::EmitStoppedEvent(StoppedEvent event)
{
    LogFuncEntry();

    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            MIProtocol::Printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",times=\"%u\",frame={%s}\n",
                event.threadId, (unsigned int)event.breakpoint.id, (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            break;
        }
        case StopStep:
        {
            MIProtocol::Printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            break;
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
            break;
        }
        case StopPause:
        {
            // When async break happens, this should be reason="interrupted".
            // But MIEngine in Visual Studio accepts only reason="signal-received",signal-name="SIGINT".
            MIProtocol::Printf("*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            break;
        }
        case StopEntry:
        {
            MIProtocol::Printf("*stopped,reason=\"entry-point-hit\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            break;
        }
        default:
            return;
    }

    MIProtocol::Printf("(gdb)\n");
}

void MIProtocol::EmitExitedEvent(ExitedEvent event)
{
    LogFuncEntry();

    MIProtocol::Printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", event.exitCode);
    MIProtocol::Printf("(gdb)\n");
}

void MIProtocol::EmitContinuedEvent(int threadId)
{
    LogFuncEntry();
}

void MIProtocol::EmitThreadEvent(ThreadEvent event)
{
    LogFuncEntry();

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
    LogFuncEntry();

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
    LogFuncEntry();

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
        IfFailRet(m_debugger->Continue(-1));
        output = "^running";
        return S_OK;
    } },
    { "exec-interrupt", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_debugger->Pause());
        output = "^done";
        return S_OK;
    } },
    { "break-insert", [this](const std::vector<std::string> &unmutable_args, std::string &output) -> HRESULT {
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
        DeleteFunctionBreakpoints(ids);
        return S_OK;
    } },
    { "break-condition", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        bool ok;
        int id = ParseInt(args.at(0), ok);
        if (!ok)
        {
            output = "Unknown breakpoint id";
            return E_FAIL;
        }

        HRESULT Status = SetBreakpointCondition(id, args.at(1));
        if (SUCCEEDED(Status))
            return Status;

        return SetFunctionBreakpointCondition(id, args.at(1));
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
        // TODO: print successful result
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
        int evalFlags = GetIntArg(args, "--evalFlags", 0);

        std::string varName = args.at(0);
        std::string varExpr = args.at(1);
        if (varExpr == "*" && args.size() >= 3)
            varExpr = args.at(2);

        return CreateVar(threadId, level, evalFlags, varName, varExpr, output);
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
        IfFailRet(m_debugger->Launch(m_fileExec, m_execArgs, {}, "", true));
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
    { "gdb-show", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() != 1
            || args.at(0) != "just-my-code")
            return E_FAIL;

        std::ostringstream ss;
        ss << "value=\"" << (m_debugger->IsJustMyCode() ? "1" : "0") << "\"";
        output = ss.str();
        return S_OK;
    }},
    { "interpreter-exec", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return S_OK;
    }},
    { "break-exception-insert", [this](const vector<string> &args, string &output) -> HRESULT {
        // That's all info about MI "-break-exception-insert" feature:
        // https://sourceware.org/gdb/onlinedocs/gdb/GDB_002fMI.html#GDB_002fMI
        // https://raw.githubusercontent.com/gregg-miskelly/MIEngine/f5f22f53908644aacffdc3f843fba20b639d07bb/src/MICore/MICommandFactory.cs
        // https://github.com/OmniSharp/omnisharp-vscode/files/626936/vscodelog.txt
        if (args.size() < 2) {
            output = "Command usage: -break-exception-insert [--mda] <unhandled|user-unhandled|throw|throw+user-unhandled> *|<Exception names>";
            return E_INVALIDARG;
        }

        size_t i = 0;
        ExceptionBreakMode filterValue;
        if (args.at(i) == "--mda") {
            filterValue.category = ExceptionBreakCategory::MDA;
            ++i;
        }

        // Unavailale for changing by user
        if (args.at(i).compare("unhandled") == 0) {
            return S_OK;
        }

        if (args.at(i).compare("user-unhandled") == 0) {
            filterValue.setUserUnhandled();
        }

        if (args.at(i).compare("throw") == 0 ) {
            filterValue.setThrow();
        }

        if (args.at(i).compare("throw+user-unhandled") == 0) {
            filterValue.setThrow();
            filterValue.setUserUnhandled();
        }

        if (!filterValue.AnyUser()) {
            output = "Command requires only:'unhandled','user-unhandled','throw','throw+user-unhandled' arguments as an exception stages";
            return E_FAIL;
        }

        // Exception names example:
        // And vsdbg have common numbers for all type of breakpoints
        //-break-exception-insert throw A B C
        //^done,bkpt=[{number="1"},{number="2"},{number="3"}]
        //(gdb)
        //-break-insert Class1.cs:1
        //^done,bkpt={number="4",type="breakpoint",disp="keep",enabled="y"}
        //(gdb)
        const vector<string> names(args.begin() + (i + 1), args.end());
        return InsertExceptionBreakpoints(filterValue, names, output);
    }},
    { "break-exception-delete", [this](const vector<string> &args, string &output) -> HRESULT {
        if (args.empty()) {
            output = "Command usage: -break-exception-delete <Exception indexes>";
            return E_INVALIDARG;
        }
        unordered_set<uint32_t> indexes;
        for (const string &id : args)
        {
            bool isTrue = false;
            int value = ParseInt(id, isTrue);
            if (isTrue) {
                indexes.insert(value);
            }
            else {
                output = "Invalid argument:'"+ id + "'";
                return E_INVALIDARG;
            }
        }
        return DeleteExceptionBreakpoints(indexes, output);
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

        IfFailRet(m_debugger->SetVariableByExpression(frameId, variable, varExpr, output));

        output = "value=\"" + MIProtocol::EscapeMIValue(output) + "\"";

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

bool MIProtocol::ParseLine(const std::string &str,
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
        args.push_back(result); // name
        args.push_back(tokenizer.Remain()); // expression
        return true;
    }
    else if (cmd == "break-condition")
    {
        tokenizer.Next(result);
        args.push_back(result); // id
        args.push_back(tokenizer.Remain()); // expression
        return true;
    }

    while (tokenizer.Next(result))
        args.push_back(result);

    return true;
}

void MIProtocol::CommandLoop()
{
    std::string token;

    Printf("(gdb)\n");

    while (!m_exit)
    {
        token.clear();
        std::string input;

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
            if (output.empty())
            {
                Printf("%s^error,msg=\"Error: 0x%08x\"\n", token.c_str(), hr);
            }
            else
            {
                Printf("%s^error,msg=\"%s\"\n", token.c_str(), MIProtocol::EscapeMIValue(output).c_str());
            }
        }
        Printf("(gdb)\n");
    }

    if (!m_exit)
        m_debugger->Disconnect(Debugger::DisconnectTerminate);

    Printf("%s^exit\n", token.c_str());
    Printf("(gdb)\n");
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

    if (n >= int(sizeof(buffer)))
    {
        strbuffer.resize(n);

        va_start(arg, fmt);
        n = vsnprintf(&strbuffer[0], strbuffer.size() + 1, fmt, arg);
        va_end(arg);

        if (n < 0 || n > int(strbuffer.size()))
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
