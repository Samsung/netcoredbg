// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/frames.h"
#include "utils/platform.h"
#include "utils/torelease.h"
#include "protocols/miprotocol.h"
#include "protocols/protocol_utils.h"
#include "tokenizer.h"
#include "utils/filesystem.h"

#include <sstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <iomanip>

#include "utils/logger.h"

namespace netcoredbg
{

extern template class EscapedString<MIProtocol::MIProtocolChars>;
extern template std::ostream& operator<<(std::ostream&, const EscapedString<MIProtocol::MIProtocolChars>&);

typedef std::function<HRESULT(
    const std::vector<std::string> &args,
    std::string &output)> CommandCallback;

static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output)
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

// print last printBpCount breakpoints into output
static HRESULT PrintExceptionBreakpoints(const std::vector<Breakpoint> &breakpoints, size_t printBpCount, std::string &output)
{
    if (printBpCount > breakpoints.size())
        return E_FAIL;

    if (printBpCount == 0 || breakpoints.empty())
    {
        output = "^done";
        return S_OK;
    }

    std::ostringstream ss;
    size_t bpIndex = breakpoints.size() - printBpCount;
    ss << "{number=\"" << breakpoints[bpIndex].id << "\"}";
    for (++bpIndex; bpIndex < breakpoints.size(); ++bpIndex)
        ss << ",{number=\"" << breakpoints[bpIndex].id << "\"}";

    if (printBpCount > 1)
        output = "^done,bkpt=[" + ss.str() + "]";
    else
        output = "^done,bkpt=" + ss.str();

    return S_OK;
}

void MIProtocol::EmitBreakpointEvent(const BreakpointEvent &event)
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
                                IDebugger::StepType stepType)
{
    ThreadId threadId{ ProtocolUtils::GetIntArg(args, "--thread", int(m_sharedDebugger->GetLastStoppedThreadId())) };
    HRESULT Status;
    IfFailRet(m_sharedDebugger->StepCommand(threadId, stepType));
    m_vars.clear(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
    output = "^running";
    return S_OK;
}

static HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output)
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
    if (stackFrame.id)
        ss << ",addr=\"" << ProtocolUtils::AddrToString(stackFrame.addr) << "\"";

    output = ss.str();

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT MIProtocol::PrintFrames(ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame)
{
    HRESULT Status;
    std::ostringstream ss;

    int totalFrames = 0;
    std::vector<StackFrame> stackFrames;
    IfFailRet(m_sharedDebugger->GetStackTrace(threadId,
        lowFrame, int(highFrame) - int(lowFrame), stackFrames, totalFrames));

    int currentFrame = int(lowFrame);

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

static HRESULT PrintVariables(const std::vector<Variable> &variables, std::string &output)
{
    std::ostringstream ss;
    ss << "variables=[";
    const char *sep = "";

    for (const Variable &var : variables)
    {
        ss << sep;
        sep = ",";

        ss << "{name=\"" << MIProtocol::EscapeMIValue(var.name) << "\"";
        ss << ",value=\"" << MIProtocol::EscapeMIValue(var.value) << "\"";
        ss << "}";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

static void PrintVar(const std::string &varobjName, Variable &v, ThreadId threadId, int print_values, std::string &output)
{
    std::ostringstream ss;

    std::string attributes;
    if (v.editable)
        attributes = "editable";
    else
        attributes = "noneditable";

    ss << "name=\"" << varobjName << "\",";
    if (print_values)
    {
        ss << "value=\"" << MIProtocol::EscapeMIValue(v.value) << "\",";
    }
    ss << "attributes=\"" << attributes << "\",";
    ss << "exp=\"" << MIProtocol::EscapeMIValue(v.name.empty() ? v.evaluateName : v.name) << "\",";
    ss << "numchild=\"" << v.namedVariables << "\",";
    ss << "type=\"" << v.type << "\",";
    ss << "thread-id=\"" << int(threadId) << "\"";

    output = ss.str();
}

HRESULT MIProtocol::PrintNewVar(const std::string& varobjName, Variable &v, ThreadId threadId, FrameLevel level, int print_values, std::string &output)
{
    if (m_vars.size() == std::numeric_limits<unsigned>::max())
        return E_FAIL;

    std::string name;
    if (varobjName.empty() || varobjName == "-")
    {
        name = "var" + std::to_string(m_vars.size() + 1);
    }
    else
    {
        name = varobjName;
    }

    m_vars[name] = MIVariable{v, threadId, level};

    PrintVar(name, v, threadId, print_values, output);

    return S_OK;
}

HRESULT MIProtocol::CreateVar(ThreadId threadId, FrameLevel level, int evalFlags, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    FrameId frameId(threadId, level);
    Variable variable(evalFlags);
    IfFailRet(m_sharedDebugger->Evaluate(frameId, expression, variable, output));

    int print_values = 1;
    return PrintNewVar(varobjName, variable, threadId, level, print_values, output);
}

HRESULT MIProtocol::DeleteVar(const std::string &varobjName)
{
    // Note:
    // * IDE could delete var objects that was created by `var-create`, when we already cleared m_vars.
    //       This happens because IDE will receive continue/step command status after we already cleared m_vars.
    // * IDE could ignore var objects created by `var-list-children`. In theory, m_vars should
    //       have tree-like structure and delete all related var objects in case root was deleted.
    // * IDE must not request old var object data after receive successful return code on continue/step command.
    //       Debugger can't provide any data by old var objects in this case, since old data have inconsistent state.
    //       This is the reason why we don't hold old data. IDE must create new var objects for each stop point.
    // * IDE should not care about `var-delete` return status, but just in case return S_OK.
    m_vars.erase(varobjName);
    return S_OK;
}

HRESULT MIProtocol::FindVar(const std::string &varobjName, MIVariable &variable)
{
    auto it = m_vars.find(varobjName);
    if (it == m_vars.end())
        return E_FAIL;

    variable = it->second;

    return S_OK;
}

void MIProtocol::Cleanup()
{
    m_vars.clear(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
    m_lineBreakpoints.clear();
    m_funcBreakpoints.clear();
    m_exceptionBreakpoints.clear();
}

HRESULT MIProtocol::PrintChildren(std::vector<Variable> &children, ThreadId threadId, FrameLevel level, int print_values, bool has_more, std::string &output)
{
    HRESULT Status;
    std::ostringstream ss;
    ss << "numchild=\"" << children.size() << "\"";

    if (children.empty())
    {
        output = ss.str();
        return S_OK;
    }
    ss << ",children=[";

    const char *sep = "";
    for (auto &child : children)
    {
        std::string varout;
        std::string minus("-");
        IfFailRet(PrintNewVar(minus, child, threadId, level, print_values, varout));

        ss << sep;
        sep = ",";
        ss << "child={" << varout << "}";
    }

    ss << "]";
    ss << ",has_more=\"" << (has_more ? 1 : 0) << "\"";
    output = ss.str();

    return S_OK;
}

HRESULT MIProtocol::ListChildren(int childStart, int childEnd, const MIVariable &miVariable, int print_values, std::string &output)
{
    HRESULT Status;
    std::vector<Variable> variables;

    bool has_more = false;

    if (miVariable.variable.variablesReference > 0)
    {
        IfFailRet(m_sharedDebugger->GetVariables(miVariable.variable.variablesReference, VariablesNamed, childStart, childEnd - childStart, variables));
        has_more = childEnd < m_sharedDebugger->GetNamedVariables(miVariable.variable.variablesReference);
        for (auto &child : variables)
        {
            child.editable = miVariable.variable.editable;
        }
    }

    return PrintChildren(variables, miVariable.threadId, miVariable.level, print_values, has_more, output);
}

HRESULT MIProtocol::SetLineBreakpoint(
    const std::string &module,
    const std::string &filename,
    int linenum,
    const std::string &condition,
    Breakpoint &breakpoint)
{
    HRESULT Status;

    auto &breakpointsInSource = m_lineBreakpoints[filename];
    std::vector<LineBreakpoint> lineBreakpoints;
    for (auto it : breakpointsInSource)
        lineBreakpoints.push_back(it.second);

    lineBreakpoints.emplace_back(module, linenum, condition);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(m_sharedDebugger->SetLineBreakpoints(filename, lineBreakpoints, breakpoints));

    // Note, SetLineBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "lineBreakpoints".
    breakpoint = breakpoints.back();
    breakpointsInSource.insert(std::make_pair(breakpoint.id, std::move(lineBreakpoints.back())));
    return S_OK;
}

HRESULT MIProtocol::SetFuncBreakpoint(const std::string &module, const std::string &funcname, const std::string &params,
                                      const std::string &condition, Breakpoint &breakpoint)
{
    HRESULT Status;

    std::vector<FuncBreakpoint> funcBreakpoints;
    for (const auto &it : m_funcBreakpoints)
        funcBreakpoints.push_back(it.second);

    funcBreakpoints.emplace_back(module, funcname, params, condition);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(m_sharedDebugger->SetFuncBreakpoints(funcBreakpoints, breakpoints));

    // Note, SetFuncBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "funcBreakpoints".
    breakpoint = breakpoints.back();
    m_funcBreakpoints.insert(std::make_pair(breakpoint.id, std::move(funcBreakpoints.back())));
    return S_OK;
}

// Note, exceptionBreakpoints data will be invalidated by this call.
HRESULT MIProtocol::SetExceptionBreakpoints(/* [in] */ std::vector<ExceptionBreakpoint> &exceptionBreakpoints, /* [out] */ std::vector<Breakpoint> &breakpoints)
{
    HRESULT Status;

    std::vector<ExceptionBreakpoint> excBreakpoints;
    for (const auto &it : m_exceptionBreakpoints)
        excBreakpoints.push_back(it.second);

    excBreakpoints.insert(excBreakpoints.end(), // Don't copy, but move exceptionBreakpoints into excBreakpoints.
                          std::make_move_iterator(exceptionBreakpoints.begin()), std::make_move_iterator(exceptionBreakpoints.end()));

    IfFailRet(m_sharedDebugger->SetExceptionBreakpoints(excBreakpoints, breakpoints));

    for (size_t i = m_exceptionBreakpoints.size(); i < breakpoints.size(); ++i)
        m_exceptionBreakpoints.insert(std::make_pair(breakpoints[i].id, std::move(excBreakpoints[i])));

    return S_OK;
}

HRESULT MIProtocol::SetLineBreakpointCondition(uint32_t id, const std::string &condition)
{
    // For each file
    for (auto &breakpointsIter : m_lineBreakpoints)
    {
        std::unordered_map<uint32_t, LineBreakpoint> &fileBreakpoints = breakpointsIter.second;

        // Find breakpoint with specified id in this file
        const auto &sbIter = fileBreakpoints.find(id);
        if (sbIter == fileBreakpoints.end())
            continue;

        // Modify breakpoint condition
        sbIter->second.condition = condition;

        // Gather all breakpoints in this file
        std::vector<LineBreakpoint> existingBreakpoints;
        existingBreakpoints.reserve(fileBreakpoints.size());
        for (const auto &it : fileBreakpoints)
            existingBreakpoints.emplace_back(it.second);

        // Update breakpoints data for this file
        const std::string &filename = breakpointsIter.first;
        std::vector<Breakpoint> tmpBreakpoints;
        return m_sharedDebugger->SetLineBreakpoints(filename, existingBreakpoints, tmpBreakpoints);
    }

    return E_FAIL;
}

HRESULT MIProtocol::SetFuncBreakpointCondition(uint32_t id, const std::string &condition)
{
    const auto &fbIter = m_funcBreakpoints.find(id);
    if (fbIter == m_funcBreakpoints.end())
        return E_FAIL;

    fbIter->second.condition = condition;

    std::vector<FuncBreakpoint> existingFuncBreakpoints;
    existingFuncBreakpoints.reserve(m_funcBreakpoints.size());
    for (const auto &fb : m_funcBreakpoints)
        existingFuncBreakpoints.emplace_back(fb.second);

    std::vector<Breakpoint> tmpBreakpoints;
    return m_sharedDebugger->SetFuncBreakpoints(existingFuncBreakpoints, tmpBreakpoints);
}

static void ParseBreakpointIndexes(const std::vector<std::string> &args, std::function<void(const std::unordered_set<uint32_t> &ids)> cb)
{
    std::unordered_set<uint32_t> ids;
    for (const std::string &idStr : args)
    {
        bool ok;
        int id = ProtocolUtils::ParseInt(idStr, ok);
        if (ok)
            ids.insert(id);
    }
    if (!ids.empty())
        cb(ids);
}

void MIProtocol::DeleteLineBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    for (auto &breakpointsIter : m_lineBreakpoints)
    {
        std::size_t initialSize = breakpointsIter.second.size();
        std::vector<LineBreakpoint> remainingBreakpoints;
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
        m_sharedDebugger->SetLineBreakpoints(filename, remainingBreakpoints, tmpBreakpoints);
    }
}

void MIProtocol::DeleteFuncBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    std::size_t initialSize = m_funcBreakpoints.size();
    std::vector<FuncBreakpoint> remainingFuncBreakpoints;
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
    m_sharedDebugger->SetFuncBreakpoints(remainingFuncBreakpoints, tmpBreakpoints);
}

void MIProtocol::DeleteExceptionBreakpoints(const std::unordered_set<uint32_t> &ids)
{
    std::size_t initialSize = m_exceptionBreakpoints.size();
    std::vector<ExceptionBreakpoint> remainingExceptionBreakpoints;
    for (auto it = m_exceptionBreakpoints.begin(); it != m_exceptionBreakpoints.end();)
    {
        if (ids.find(it->first) == ids.end())
        {
            remainingExceptionBreakpoints.push_back(it->second);
            ++it;
        }
        else
            it = m_exceptionBreakpoints.erase(it);
    }

    if (initialSize == m_exceptionBreakpoints.size())
        return;

    std::vector<Breakpoint> tmpBreakpoints;
    m_sharedDebugger->SetExceptionBreakpoints(remainingExceptionBreakpoints, tmpBreakpoints);
}

void MIProtocol::EmitStoppedEvent(const StoppedEvent &event)
{
    LogFuncEntry();

    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            MIProtocol::Printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",times=\"%u\",frame={%s}\n",
                int(event.threadId), (unsigned int)event.breakpoint.id, (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            break;
        }
        case StopStep:
        {
            MIProtocol::Printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopException:
        {
            MIProtocol::Printf("*stopped,reason=\"exception-received\",exception-name=\"%s\",exception=\"%s\",exception-stage=\"%s\",exception-category=\"%s\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.exception_name.c_str(),
                MIProtocol::EscapeMIValue(event.exception_message.empty() ? event.text : event.exception_message).c_str(),
                event.exception_stage.c_str(),
                event.exception_category.c_str(),
                int(event.threadId),
                frameLocation.c_str());
            break;
        }
        case StopPause:
        {
            // When async break happens, this should be reason="interrupted".
            // But MIEngine in Visual Studio accepts only reason="signal-received",signal-name="SIGINT".
            MIProtocol::Printf("*stopped,reason=\"signal-received\",signal-name=\"SIGINT\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                int(event.threadId), frameLocation.c_str());
            break;
        }
        case StopEntry:
        {
            MIProtocol::Printf("*stopped,reason=\"entry-point-hit\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                int(event.threadId), frameLocation.c_str());
            break;
        }
        default:
            return;
    }

    MIProtocol::Printf("(gdb)\n");
}

void MIProtocol::EmitExitedEvent(const ExitedEvent &event)
{
    LogFuncEntry();

    MIProtocol::Printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", event.exitCode);
    MIProtocol::Printf("(gdb)\n");
}

void MIProtocol::EmitContinuedEvent(ThreadId threadId)
{
    LogFuncEntry();
}

void MIProtocol::EmitThreadEvent(const ThreadEvent &event)
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
    MIProtocol::Printf("=%s,id=\"%i\"\n", reasonText, int(event.threadId));
}

void MIProtocol::EmitModuleEvent(const ModuleEvent &event)
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

void MIProtocol::EmitOutputEvent(OutputCategory category, string_view output, string_view source)
{
    LogFuncEntry();

#if 0 
    static const string_view categories[] = {"console", "stdout", "stderr"};

    assert(category == OutputConsole || category == OutputStdOut || category == OutputStdErr);
    const string_view& name = categories[category];

    cout << name...   // TODO enable this after fixing plugin
#endif

    std::lock_guard<std::mutex> lock(m_outMutex);

    cout << "=message,text=\"" << EscapeMIValue(output) << "\",send-to=\"output-window\"";

    if (!source.empty())
        cout << ",source=\"" << EscapeMIValue(source) << "\"";

    cout << "\n";
    cout.flush();
}

HRESULT MIProtocol::HandleCommand(const std::string& command, const std::vector<std::string> &args, std::string &output)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status = S_OK;

        std::vector<Thread> threads;
        IfFailRet(m_sharedDebugger->GetThreads(threads));

        std::ostringstream ss;

        ss << "threads=[";

        const char *sep = "";
        for (const Thread& thread : threads)
        {
            ss << sep << "{id=\"" << int(thread.id)
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
        IfFailRet(m_sharedDebugger->Continue(ThreadId::AllThreads));
        m_vars.clear(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
        output = "^running";
        return S_OK;
    } },
    { "exec-interrupt", [this](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_sharedDebugger->Pause());
        output = "^done";
        return S_OK;
    } },
    { "break-insert", [this](const std::vector<std::string> &unmutable_args, std::string &output) -> HRESULT {
        HRESULT Status = E_FAIL;
        Breakpoint breakpoint;
        std::vector<std::string> args = unmutable_args;

        ProtocolUtils::StripArgs(args);

        BreakType bt = ProtocolUtils::GetBreakpointType(args);

        if (bt == BreakType::Error)
        {
            output = "Wrong breakpoint specified";
            return E_FAIL;
        }

        if (bt == BreakType::LineBreak)
        {
            struct LineBreak lb;

            if (ProtocolUtils::ParseBreakpoint(args, lb)
                && SUCCEEDED(SetLineBreakpoint(lb.module, lb.filename, lb.linenum, lb.condition, breakpoint)))
                Status = S_OK;
        }
        else if (bt == BreakType::FuncBreak)
        {
            struct FuncBreak fb;

            if (ProtocolUtils::ParseBreakpoint(args, fb)
                && SUCCEEDED(SetFuncBreakpoint(fb.module, fb.funcname, fb.params, fb.condition, breakpoint)))
                Status = S_OK;
        }

        if (Status == S_OK)
            PrintBreakpoint(breakpoint, output);
        else
            output = "Unknown breakpoint location format";

        return Status;
    } },
    { "break-exception-insert", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 2)
        {
            output = "Command usage: -break-exception-insert [--mda] <unhandled|user-unhandled|throw|throw+user-unhandled> *|<Exception names>";
            return E_INVALIDARG;
        }

        size_t i = 0;
        ExceptionCategory category;
        if (args.at(i) == "--mda")
        {
            category = ExceptionCategory::MDA;
            ++i;
        }
        else
            category = ExceptionCategory::CLR;

        static std::unordered_map<std::string, ExceptionBreakpointFilter> MIFilters{
            {"throw",                ExceptionBreakpointFilter::THROW},
            {"user-unhandled",       ExceptionBreakpointFilter::USER_UNHANDLED},
            {"throw+user-unhandled", ExceptionBreakpointFilter::THROW_USER_UNHANDLED},
            {"unhandled",            ExceptionBreakpointFilter::UNHANDLED}};

        auto findFilter = MIFilters.find(args.at(i));
        if (findFilter == MIFilters.end())
        {
            output = "Command requires only: 'unhandled', 'user-unhandled', 'throw' and 'throw+user-unhandled' argument as an exception stage";
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

        size_t newBpCount = exceptionBreakpoints.size();
        if (newBpCount == 0)
            return E_INVALIDARG;

        HRESULT Status;
        std::vector<Breakpoint> breakpoints;
        // `breakpoints` will return all configured exception breakpoints, not only configured by this command.
        // Note, exceptionBreakpoints data will be invalidated by this call.
        IfFailRet(SetExceptionBreakpoints(exceptionBreakpoints, breakpoints));
        // Print only breakpoints configured by this command (last newBpCount entries).
        IfFailRet(PrintExceptionBreakpoints(breakpoints, newBpCount, output));

        return S_OK;
    }},
    { "break-delete", [this](const std::vector<std::string> &args, std::string &) -> HRESULT {
        ParseBreakpointIndexes(args, [this](const std::unordered_set<uint32_t> &ids)
        {
            DeleteLineBreakpoints(ids);
            DeleteFuncBreakpoints(ids);
        });
        return S_OK;
    } },
    { "break-exception-delete", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        ParseBreakpointIndexes(args, [this](const std::unordered_set<uint32_t> &ids)
        {
            DeleteExceptionBreakpoints(ids);
        });
        return S_OK;
    }},
    { "break-condition", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        bool ok;
        int id = ProtocolUtils::ParseInt(args.at(0), ok);
        if (!ok)
        {
            output = "Unknown breakpoint id";
            return E_FAIL;
        }

        HRESULT Status = SetLineBreakpointCondition(id, args.at(1));
        if (SUCCEEDED(Status))
            return Status;

        return SetFuncBreakpointCondition(id, args.at(1));
    } },
    { "exec-step", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, IDebugger::StepType::STEP_IN);
    }},
    { "exec-next", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, IDebugger::StepType::STEP_OVER);
    }},
    { "exec-finish", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, IDebugger::StepType::STEP_OUT);
    }},
    { "exec-abort", [this](const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectTerminate);
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
        int pid = ProtocolUtils::ParseInt(args.at(0), ok);
        if (!ok) return E_INVALIDARG;

        m_sharedDebugger->Initialize();
        IfFailRet(m_sharedDebugger->Attach(pid));
        IfFailRet(m_sharedDebugger->ConfigurationDone());
        // TODO: print successful result
        return S_OK;
    }},
    { "target-detach", [this](const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectDetach);
        return S_OK;
    }},
    { "stack-list-frames", [this](const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        std::vector<std::string> args = args_orig;
        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(m_sharedDebugger->GetLastStoppedThreadId())) };
        int lowFrame = 0;
        int highFrame = FrameLevel::MaxFrameLevel;
        ProtocolUtils::StripArgs(args);
        ProtocolUtils::GetIndices(args, lowFrame, highFrame);
        return PrintFrames(threadId, output, FrameLevel{lowFrame}, FrameLevel{highFrame});
    }},
    { "stack-list-variables", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(m_sharedDebugger->GetLastStoppedThreadId())) };
        StackFrame stackFrame(threadId, FrameLevel{ProtocolUtils::GetIntArg(args, "--frame", 0)}, "");
        std::vector<Scope> scopes;
        std::vector<Variable> variables;
        IfFailRet(m_sharedDebugger->GetScopes(stackFrame.id, scopes));
        if (!scopes.empty() && scopes[0].variablesReference != 0)
        {
            IfFailRet(m_sharedDebugger->GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables));
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

        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(m_sharedDebugger->GetLastStoppedThreadId())) };
        FrameLevel level { ProtocolUtils::GetIntArg(args, "--frame", 0) };
        int evalFlags = ProtocolUtils::GetIntArg(args, "--evalFlags", 0);

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

        int childStart = 0;
        int childEnd = INT_MAX;
        ProtocolUtils::StripArgs(args);
        ProtocolUtils::GetIndices(args, childStart, childEnd);
        std::string varName = args.at(0);
        HRESULT Status;
        MIVariable miVariable;
        IfFailRet(FindVar(varName, miVariable));

        return ListChildren(childStart, childEnd, miVariable, print_values, output);
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

        m_sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectTerminate);

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
        m_sharedDebugger->Initialize();
        // Note, in case of MI protocol, we enable stop at entry point all the time from debugger side,
        // MIEngine will continue debuggee process at entry point stop event if IDE configured to ignore it.
        IfFailRet(m_sharedDebugger->Launch(m_fileExec, m_execArgs, {}, "", true));
        Status = m_sharedDebugger->ConfigurationDone();
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
        if (args.size() != 2)
            return E_FAIL;

        if (args.at(0) == "just-my-code")
            m_sharedDebugger->SetJustMyCode(args.at(1) == "1");
        else if (args.at(0) == "enable-step-filtering")
            m_sharedDebugger->SetStepFiltering(args.at(1) == "1");
        else if (args.at(0) == "enable-hot-reload")
            return m_sharedDebugger->SetHotReload(args.at(1) == "1");
        else
            return E_FAIL;

        return S_OK;
    }},
    { "gdb-show", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() != 1)
            return E_FAIL;

        std::ostringstream ss;

        if (args.at(0) == "just-my-code")
            ss << "value=\"" << (m_sharedDebugger->IsJustMyCode() ? "1" : "0") << "\"";
        else if (args.at(0) == "enable-step-filtering")
            ss << "value=\"" << (m_sharedDebugger->IsStepFiltering() ? "1" : "0") << "\"";
        else
            return E_FAIL;

        output = ss.str();
        return S_OK;
    }},
    { "interpreter-exec", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return S_OK;
    }},
    { "var-show-attributes", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        MIVariable miVariable;
        std::string varName = args.at(0);
        std::string attributes;

        IfFailRet(FindVar(varName, miVariable));
        if (miVariable.variable.editable)
            attributes = "editable";
        else
            attributes = "noneditable";

        output = "status=\"" + attributes + "\"";
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

        MIVariable miVariable;
        IfFailRet(FindVar(varName, miVariable));

        FrameId frameId(miVariable.threadId, miVariable.level);
        IfFailRet(m_sharedDebugger->SetExpression(frameId, miVariable.variable.evaluateName, miVariable.variable.evalFlags, varExpr, output));

        output = "value=\"" + MIProtocol::EscapeMIValue(output) + "\"";

        return S_OK;
    }},
    { "var-evaluate-expression", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        if (args.size() != 1)
        {
            output = "Command requires 1 argument";
            return E_FAIL;
        }

        std::string varName = args.at(0);

        MIVariable miVariable;
        IfFailRet(FindVar(varName, miVariable));
        FrameId frameId(miVariable.threadId, miVariable.level);
        Variable variable(miVariable.variable.evalFlags);
        IfFailRet(m_sharedDebugger->Evaluate(frameId, miVariable.variable.evaluateName, variable, output));

        output = "value=\"" + MIProtocol::EscapeMIValue(variable.value) + "\"";
        return S_OK;
    }},
    { "apply-deltas", [this](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        if (args.size() != 5)
        {
            output = "Command requires 5 arguments";
            return E_FAIL;
        }

        std::string dllFileName = args.at(0);
        std::string deltaMD = args.at(1);
        std::string deltaIL = args.at(2);
        std::string deltaPDB = args.at(3);
        std::string lineUpdates = args.at(4);

        IfFailRet(m_sharedDebugger->HotReloadApplyDeltas(dllFileName, deltaMD, deltaIL, deltaPDB, lineUpdates));

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

static bool ParseLine(const std::string &str, std::string &token, std::string &cmd, std::vector<std::string> &args)
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

        std::getline(cin, input);
        if (input.empty() && cin.eof())
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
        m_sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectTerminate);

    Printf("%s^exit\n", token.c_str());
    Printf("(gdb)\n");
}

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
    cout << out;
    cout.flush(); // TODO too frequent flush
}


struct MIProtocol::MIProtocolChars
{
    // list of characters to be replaced
    static const char forbidden_chars[];

    // substitutions (except of '\\' prefix)
    static const string_view subst_chars[];

    static constexpr const char escape_char = '\\';
};


const char MIProtocol::MIProtocolChars::forbidden_chars[] = "\"\\\0\a\b\f\n\r\t\v";
const string_view MIProtocol::MIProtocolChars::subst_chars[] = { "\\\"", "\\\\", "\\0", "\\a", "\\b", "\\f", "\\n", "\\r", "\\t", "\\v" };
const char MIProtocol::MIProtocolChars::escape_char;


template class EscapedString<MIProtocol::MIProtocolChars>;
template std::ostream& operator<<(std::ostream& os, const EscapedString<MIProtocol::MIProtocolChars>& estr);


} // namespace netcoredbg
