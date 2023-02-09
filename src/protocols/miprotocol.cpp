// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/frames.h"
#include "utils/platform.h"
#include "utils/torelease.h"
#include "protocols/miprotocol.h"
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

static HRESULT StepCommand(std::shared_ptr<IDebugger> &sharedDebugger, MIProtocol::VariablesHandle &variablesHandle,
                           const std::vector<std::string> &args, IDebugger::StepType stepType, std::string &output)
{
    ThreadId threadId{ ProtocolUtils::GetIntArg(args, "--thread", int(sharedDebugger->GetLastStoppedThreadId())) };
    HRESULT Status;
    IfFailRet(sharedDebugger->StepCommand(threadId, stepType));
    variablesHandle.Cleanup(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
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
           << "method-version=\"" << std::dec << stackFrame.clrAddr.methodVersion << "\","
           << "il-offset=\"" << std::dec << stackFrame.clrAddr.ilOffset
           << "\",native-offset=\"" << stackFrame.clrAddr.nativeOffset << "\"},";
    }

    ss << "func=\"" << stackFrame.name << "\"";
    if (stackFrame.id)
        ss << ",addr=\"" << ProtocolUtils::AddrToString(stackFrame.addr) << "\"";

    ss << ",active-statement-flags=\"";
    if (stackFrame.activeStatementFlags == StackFrame::ActiveStatementFlags::None)
    {
        ss << "None";
    }
    else
    {
        struct flag_t
        {
            StackFrame::ActiveStatementFlags bit;
            std::string name;
            flag_t(StackFrame::ActiveStatementFlags bit_, const std::string &name_) : bit(bit_), name(name_) {}
        };
        static const std::vector<flag_t> flagsMap
           {{StackFrame::ActiveStatementFlags::LeafFrame,          "LeafFrame"},
            {StackFrame::ActiveStatementFlags::NonLeafFrame,       "NonLeafFrame"},
            {StackFrame::ActiveStatementFlags::PartiallyExecuted,  "PartiallyExecuted"},
            {StackFrame::ActiveStatementFlags::MethodUpToDate,     "MethodUpToDate"},
            {StackFrame::ActiveStatementFlags::Stale,              "Stale"}};
        bool first = true;
        for (auto &flag : flagsMap)
        {
            if ((stackFrame.activeStatementFlags & flag.bit) == flag.bit)
            {
                ss << (first ? "":",") << flag.name;
                first = false;
            }
        }
    }
    ss << "\"";

    output = ss.str();

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

static HRESULT PrintFrames(std::shared_ptr<IDebugger> &sharedDebugger, ThreadId threadId, std::string &output, FrameLevel lowFrame, FrameLevel highFrame, bool hotReloadAwareCaller)
{
    HRESULT Status;
    std::ostringstream ss;

    int totalFrames = 0;
    std::vector<StackFrame> stackFrames;
    IfFailRet(sharedDebugger->GetStackTrace(threadId, lowFrame, int(highFrame) - int(lowFrame), stackFrames, totalFrames, hotReloadAwareCaller));

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

HRESULT MIProtocol::VariablesHandle::PrintNewVar(const std::string& varobjName, Variable &v, ThreadId threadId,
                                                 FrameLevel level, int print_values, std::string &output)
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

HRESULT MIProtocol::VariablesHandle::CreateVar(std::shared_ptr<IDebugger> &sharedDebugger, ThreadId threadId, FrameLevel level,
                                               int evalFlags, const std::string &varobjName, const std::string &expression, std::string &output)
{
    HRESULT Status;

    FrameId frameId(threadId, level);
    Variable variable(evalFlags);
    IfFailRet(sharedDebugger->Evaluate(frameId, expression, variable, output));

    int print_values = 1;
    return PrintNewVar(varobjName, variable, threadId, level, print_values, output);
}

HRESULT MIProtocol::VariablesHandle::DeleteVar(const std::string &varobjName)
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

HRESULT MIProtocol::VariablesHandle::FindVar(const std::string &varobjName, MIVariable &variable)
{
    auto it = m_vars.find(varobjName);
    if (it == m_vars.end())
        return E_FAIL;

    variable = it->second;

    return S_OK;
}

void MIProtocol::Cleanup()
{
    m_variablesHandle.Cleanup(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
    m_breakpointsHandle.Cleanup();
}

void MIProtocol::VariablesHandle::Cleanup()
{
    m_vars.clear();
}

HRESULT MIProtocol::VariablesHandle::PrintChildren(std::vector<Variable> &children, ThreadId threadId, FrameLevel level,
                                                   int print_values, bool has_more, std::string &output)
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

HRESULT MIProtocol::VariablesHandle::ListChildren(std::shared_ptr<IDebugger> &sharedDebugger, int childStart, int childEnd,
                                                  const MIVariable &miVariable, int print_values, std::string &output)
{
    HRESULT Status;
    std::vector<Variable> variables;

    bool has_more = false;

    if (miVariable.variable.variablesReference > 0)
    {
        IfFailRet(sharedDebugger->GetVariables(miVariable.variable.variablesReference, VariablesNamed, childStart, childEnd - childStart, variables));
        has_more = childEnd < sharedDebugger->GetNamedVariables(miVariable.variable.variablesReference);
        for (auto &child : variables)
        {
            child.editable = miVariable.variable.editable;
        }
    }

    return PrintChildren(variables, miVariable.threadId, miVariable.level, print_values, has_more, output);
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
        case ModuleRemoved:
        {
            std::ostringstream ss;
            ss << "id=\"{" << event.module.id << "}\","
               << "target-name=\"" << MIProtocol::EscapeMIValue(event.module.path) << "\","
               << "host-name=\"" << MIProtocol::EscapeMIValue(event.module.path) << "\"";
            Printf("=library-unloaded,%s\n", ss.str().c_str());
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

static HRESULT HandleCommand(std::shared_ptr<IDebugger> &sharedDebugger, BreakpointsHandle &breakpointsHandle, MIProtocol::VariablesHandle &variablesHandle,
                             std::string &fileExec, std::vector<std::string> &execArgs, const std::string& command, const std::vector<std::string> &args, std::string &output)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", [&](const std::vector<std::string> &, std::string &output){
        HRESULT Status = S_OK;

        std::vector<Thread> threads;
        IfFailRet(sharedDebugger->GetThreads(threads));

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
    { "exec-continue", [&](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(sharedDebugger->Continue(ThreadId::AllThreads));
        variablesHandle.Cleanup(); // Important, must be sync with ManagedDebugger m_sharedVariables->Clear()
        output = "^running";
        return S_OK;
    } },
    { "exec-interrupt", [&](const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(sharedDebugger->Pause(ThreadId::AllThreads));
        output = "^done";
        return S_OK;
    } },
    { "break-update-line", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        // Custom MI protocol command for line breakpoint update.
        // Command format:
        //    break-update-line ID NEW_LINE
        // where
        //    ID - ID of previously added breakpoint, that should be changed;
        //    NEW_LINE - new line number in source file.
        if (args.size() != 2)
        {
            output = "Command requires 2 arguments";
            return E_FAIL;
        }

        bool ok;
        int id = ProtocolUtils::ParseInt(args.at(0), ok);
        if (!ok)
        {
            output = "Unknown breakpoint id";
            return E_FAIL;
        }

        int linenum = ProtocolUtils::ParseInt(args.at(1), ok);
        if (!ok)
        {
            output = "Unknown breakpoint new line";
            return E_FAIL;
        }

        Breakpoint breakpoint;
        if (SUCCEEDED(breakpointsHandle.UpdateLineBreakpoint(sharedDebugger, id, linenum, breakpoint)))
        {
            PrintBreakpoint(breakpoint, output);
            return S_OK;
        }

        output = "Unknown breakpoint location, breakpoint was not updated";
        return E_FAIL;
    } },
    { "break-insert", [&](const std::vector<std::string> &unmutable_args, std::string &output) -> HRESULT {
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
                && SUCCEEDED(breakpointsHandle.SetLineBreakpoint(sharedDebugger, lb.module, lb.filename, lb.linenum, lb.condition, breakpoint)))
                Status = S_OK;
        }
        else if (bt == BreakType::FuncBreak)
        {
            struct FuncBreak fb;

            if (ProtocolUtils::ParseBreakpoint(args, fb)
                && SUCCEEDED(breakpointsHandle.SetFuncBreakpoint(sharedDebugger, fb.module, fb.funcname, fb.params, fb.condition, breakpoint)))
                Status = S_OK;
        }

        if (Status == S_OK)
            PrintBreakpoint(breakpoint, output);
        else
            output = "Unknown breakpoint location format";

        return Status;
    } },
    { "break-exception-insert", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
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
        IfFailRet(breakpointsHandle.SetExceptionBreakpoints(sharedDebugger, exceptionBreakpoints, breakpoints));
        // Print only breakpoints configured by this command (last newBpCount entries).
        IfFailRet(PrintExceptionBreakpoints(breakpoints, newBpCount, output));

        return S_OK;
    }},
    { "break-delete", [&](const std::vector<std::string> &args, std::string &) -> HRESULT {
        ParseBreakpointIndexes(args, [&](const std::unordered_set<uint32_t> &ids)
        {
            breakpointsHandle.DeleteLineBreakpoints(sharedDebugger, ids);
            breakpointsHandle.DeleteFuncBreakpoints(sharedDebugger, ids);
        });
        return S_OK;
    } },
    { "break-exception-delete", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        ParseBreakpointIndexes(args, [&](const std::unordered_set<uint32_t> &ids)
        {
            breakpointsHandle.DeleteExceptionBreakpoints(sharedDebugger, ids);
        });
        return S_OK;
    }},
    { "break-condition", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
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

        HRESULT Status = breakpointsHandle.SetLineBreakpointCondition(sharedDebugger, id, args.at(1));
        if (SUCCEEDED(Status))
            return Status;

        return breakpointsHandle.SetFuncBreakpointCondition(sharedDebugger, id, args.at(1));
    } },
    { "exec-step", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(sharedDebugger, variablesHandle, args, IDebugger::StepType::STEP_IN, output);
    }},
    { "exec-next", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(sharedDebugger, variablesHandle, args, IDebugger::StepType::STEP_OVER, output);
    }},
    { "exec-finish", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(sharedDebugger, variablesHandle, args, IDebugger::StepType::STEP_OUT, output);
    }},
    { "exec-abort", [&](const std::vector<std::string> &, std::string &output) -> HRESULT {
        sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectTerminate);
        return S_OK;
    }},
    { "target-attach", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        if (args.size() != 1)
        {
            output = "Command requires an argument";
            return E_INVALIDARG;
        }
        bool ok;
        int pid = ProtocolUtils::ParseInt(args.at(0), ok);
        if (!ok) return E_INVALIDARG;

        sharedDebugger->Initialize();
        IfFailRet(sharedDebugger->Attach(pid));
        IfFailRet(sharedDebugger->ConfigurationDone());
        // TODO: print successful result
        return S_OK;
    }},
    { "target-detach", [&](const std::vector<std::string> &, std::string &output) -> HRESULT {
        sharedDebugger->Disconnect(IDebugger::DisconnectAction::DisconnectDetach);
        return S_OK;
    }},
    { "stack-list-frames", [&](const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        std::vector<std::string> args = args_orig;
        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(sharedDebugger->GetLastStoppedThreadId())) };
        bool hotReloadAwareCaller = ProtocolUtils::FindAndEraseArg(args, "--hot-reload");
        int lowFrame = 0;
        int highFrame = FrameLevel::MaxFrameLevel;
        ProtocolUtils::StripArgs(args);
        ProtocolUtils::GetIndices(args, lowFrame, highFrame);
        return PrintFrames(sharedDebugger, threadId, output, FrameLevel{lowFrame}, FrameLevel{highFrame}, hotReloadAwareCaller);
    }},
    { "stack-list-variables", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(sharedDebugger->GetLastStoppedThreadId())) };
        StackFrame stackFrame(threadId, FrameLevel{ProtocolUtils::GetIntArg(args, "--frame", 0)}, "");
        std::vector<Scope> scopes;
        std::vector<Variable> variables;
        IfFailRet(sharedDebugger->GetScopes(stackFrame.id, scopes));
        if (!scopes.empty() && scopes[0].variablesReference != 0)
        {
            IfFailRet(sharedDebugger->GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables));
        }

        PrintVariables(variables, output);

        return S_OK;
    }},
    { "var-create", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        ThreadId threadId { ProtocolUtils::GetIntArg(args, "--thread", int(sharedDebugger->GetLastStoppedThreadId())) };
        FrameLevel level { ProtocolUtils::GetIntArg(args, "--frame", 0) };
        int evalFlags = ProtocolUtils::GetIntArg(args, "--evalFlags", 0);

        std::string varName = args.at(0);
        std::string varExpr = args.at(1);
        if (varExpr == "*" && args.size() >= 3)
            varExpr = args.at(2);

        return variablesHandle.CreateVar(sharedDebugger, threadId, level, evalFlags, varName, varExpr, output);
    }},
    { "var-list-children", [&](const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
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
        MIProtocol::MIVariable miVariable;
        IfFailRet(variablesHandle.FindVar(varName, miVariable));

        return variablesHandle.ListChildren(sharedDebugger, childStart, childEnd, miVariable, print_values, output);
    }},
    { "var-delete", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 1)
        {
            output = "Command requires at least 1 argument";
            return E_FAIL;
        }
        return variablesHandle.DeleteVar(args.at(0));
    }},
    { "gdb-exit", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        sharedDebugger->Disconnect(); // Terminate debuggee process if debugger ran this process and detach in case debugger was attached to it.
        return S_OK;
    }},
    { "file-exec-and-symbols", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        fileExec = args.at(0);
        return S_OK;
    }},
    { "exec-arguments", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        execArgs = args;
        return S_OK;
    }},
    { "exec-run", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        sharedDebugger->Initialize();
        // Note, in case of MI protocol, we enable stop at entry point all the time from debugger side,
        // MIEngine will continue debuggee process at entry point stop event if IDE configured to ignore it.
        IfFailRet(sharedDebugger->Launch(fileExec, execArgs, {}, "", true));
        Status = sharedDebugger->ConfigurationDone();
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
    { "gdb-set", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() != 2)
            return E_FAIL;

        if (args.at(0) == "just-my-code")
            sharedDebugger->SetJustMyCode(args.at(1) == "1");
        else if (args.at(0) == "enable-step-filtering")
            sharedDebugger->SetStepFiltering(args.at(1) == "1");
        else if (args.at(0) == "enable-hot-reload")
            return sharedDebugger->SetHotReload(args.at(1) == "1");
        else
            return E_FAIL;

        return S_OK;
    }},
    { "gdb-show", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() != 1)
            return E_FAIL;

        std::ostringstream ss;

        if (args.at(0) == "just-my-code")
            ss << "value=\"" << (sharedDebugger->IsJustMyCode() ? "1" : "0") << "\"";
        else if (args.at(0) == "enable-step-filtering")
            ss << "value=\"" << (sharedDebugger->IsStepFiltering() ? "1" : "0") << "\"";
        else
            return E_FAIL;

        output = ss.str();
        return S_OK;
    }},
    { "interpreter-exec", [](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return S_OK;
    }},
    { "var-show-attributes", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        MIProtocol::MIVariable miVariable;
        std::string varName = args.at(0);
        std::string attributes;

        IfFailRet(variablesHandle.FindVar(varName, miVariable));
        if (miVariable.variable.editable)
            attributes = "editable";
        else
            attributes = "noneditable";

        output = "status=\"" + attributes + "\"";
        return S_OK;
    }},
    { "var-assign", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
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

        MIProtocol::MIVariable miVariable;
        IfFailRet(variablesHandle.FindVar(varName, miVariable));

        FrameId frameId(miVariable.threadId, miVariable.level);
        IfFailRet(sharedDebugger->SetExpression(frameId, miVariable.variable.evaluateName, miVariable.variable.evalFlags, varExpr, output));

        output = "value=\"" + MIProtocol::EscapeMIValue(output) + "\"";

        return S_OK;
    }},
    { "var-evaluate-expression", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        if (args.size() != 1)
        {
            output = "Command requires 1 argument";
            return E_FAIL;
        }

        std::string varName = args.at(0);

        MIProtocol::MIVariable miVariable;
        IfFailRet(variablesHandle.FindVar(varName, miVariable));
        FrameId frameId(miVariable.threadId, miVariable.level);
        Variable variable(miVariable.variable.evalFlags);
        IfFailRet(sharedDebugger->Evaluate(frameId, miVariable.variable.evaluateName, variable, output));

        output = "value=\"" + MIProtocol::EscapeMIValue(variable.value) + "\"";
        return S_OK;
    }},
    { "apply-deltas", [&](const std::vector<std::string> &args, std::string &output) -> HRESULT {
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

        IfFailRet(sharedDebugger->HotReloadApplyDeltas(dllFileName, deltaMD, deltaIL, deltaPDB, lineUpdates));

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

        // Pre command action.
        if (command == "gdb-exit")
            m_exit = true;

        std::string output;
        HRESULT hr = HandleCommand(m_sharedDebugger, m_breakpointsHandle, m_variablesHandle, m_fileExec, m_execArgs, command, args, output);

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
        m_sharedDebugger->Disconnect(); // Terminate debuggee process if debugger ran this process and detach in case debugger was attached to it.

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
