#include "common.h"

#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <mutex>
#include <condition_variable>

#include "debugger.h"

using namespace std::placeholders;

// Varobj
HRESULT ListVariables(ICorDebugThread *pThread, ICorDebugFrame *pFrame, std::string &output);
HRESULT CreateVar(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &varobjName, const std::string &expression, std::string &output);
HRESULT ListChildren(int childStart, int childEnd, const std::string &name, int print_values, ICorDebugThread *pThread, ICorDebugFrame *pFrame, std::string &output);
HRESULT DeleteVar(const std::string &varobjName);

// Modules
HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range);

// Breakpoints
HRESULT DeleteBreakpoint(ULONG32 id);
HRESULT InsertBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);
ULONG32 InsertExceptionBreakpoint(const std::string &name);

// Frames
HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame);

// Main
HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);


typedef std::function<HRESULT(
    ICorDebugProcess *pProcess,
    const std::vector<std::string> &args,
    std::string &output)> CommandCallback;

HRESULT PrintThreadsState(ICorDebugController *controller, std::string &output);
HRESULT PrintFrames(ICorDebugThread *pThread, std::string &output, int lowFrame = 0, int highFrame = INT_MAX);

// Breakpoints
HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);

// Debug events
int GetLastStoppedThreadId();
void WaitProcessExited();

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

static HRESULT BreakInsertCommand(
    ICorDebugProcess *pProcess,
    const std::vector<std::string> &args,
    std::string &output)
{
    std::string filename;
    unsigned int linenum;
    ULONG32 id;
    if (ParseBreakpoint(args, filename, linenum)
        && SUCCEEDED(InsertBreakpointInProcess(pProcess, filename, linenum, id)))
    {
        PrintBreakpoint(id, output);
        return S_OK;
    }

    output = "Unknown breakpoint location format";
    return E_FAIL;
}

enum StepType {
    STEP_IN = 0,
    STEP_OVER,
    STEP_OUT
};

static HRESULT RunStep(ICorDebugThread *pThread, StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    CorDebugUnmappedStop stopMask = STOP_NONE;
    IfFailRet(pStepper->SetUnmappedStopMask(stopMask));

    ToRelease<ICorDebugStepper2> pStepper2;
    IfFailRet(pStepper->QueryInterface(IID_ICorDebugStepper2, (LPVOID *)&pStepper2));

    IfFailRet(pStepper2->SetJMC(Debugger::IsJustMyCode()));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());
        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    return S_OK;
}

static HRESULT StepCommand(ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output, StepType stepType)
{
    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
    IfFailRet(pProcess->GetThread(threadId, &pThread));
    DisableAllSteppers(pProcess);
    IfFailRet(RunStep(pThread, stepType));
    IfFailRet(pProcess->Continue(0));
    output = "^running";
    return S_OK;
}

static HRESULT ThreadInfoCommand(ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &output)
{
    if (!pProcess) return E_FAIL;
    return PrintThreadsState(pProcess, output);
}

HRESULT Debugger::HandleCommand(std::string command,
                                const std::vector<std::string> &args,
                                std::string &output)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", ThreadInfoCommand },
    { "exec-continue", [](ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &){
        if (!pProcess) return E_FAIL;
        return pProcess->Continue(0); } },
    { "exec-interrupt", [](ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &){
        if (!pProcess) return E_FAIL;
        return pProcess->Stop(0); } },
    { "break-insert", BreakInsertCommand },
    { "break-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &) -> HRESULT {
        for (const std::string &idStr : args)
        {
            bool ok;
            int id = ParseInt(idStr, ok);
            if (ok)
                DeleteBreakpoint(id);
        }
        return S_OK;
    } },
    { "exec-step", std::bind(StepCommand, _1, _2, _3, STEP_IN) },
    { "exec-next", std::bind(StepCommand, _1, _2, _3, STEP_OVER) },
    { "exec-finish", std::bind(StepCommand, _1, _2, _3, STEP_OUT) },
    { "target-attach", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        if (args.size() != 1)
        {
            output = "Command requires an argument";
            return E_INVALIDARG;
        }
        bool ok;
        int pid = ParseInt(args.at(0), ok);
        if (!ok) return E_INVALIDARG;
        IfFailRet(this->AttachToProcess(pid));
        // TODO: print succeessful result
        return S_OK;
    }},
    { "target-detach", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output) -> HRESULT {
        this->DetachFromProcess();
        return S_OK;
    }},
    { "stack-list-frames", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        std::vector<std::string> args = args_orig;
        HRESULT Status;
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));
        int lowFrame = 0;
        int highFrame = INT_MAX;
        StripArgs(args);
        GetIndices(args, lowFrame, highFrame);
        IfFailRet(PrintFrames(pThread, output, lowFrame, highFrame));
        return S_OK;
    }},
    { "stack-list-variables", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        HRESULT Status;
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, GetIntArg(args, "--frame", 0), &pFrame));

        IfFailRet(ListVariables(pThread, pFrame, output));

        return S_OK;
    }},
    { "var-create", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        HRESULT Status;

        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, GetIntArg(args, "--frame", 0), &pFrame));

        std::string var_name = args.at(0);
        std::string var_expr = args.at(1);
        if (var_expr == "*" && args.size() >= 3)
            var_expr = args.at(2);

        return CreateVar(pThread, pFrame, var_name, var_expr, output);
    }},
    { "var-list-children", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        HRESULT Status;

        std::vector<std::string> args = args_orig;

        int print_values = 0;
        int var_index = 0;
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

        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pThread->GetActiveFrame(&pFrame));
        int childStart = 0;
        int childEnd = INT_MAX;
        StripArgs(args);
        GetIndices(args, childStart, childEnd);
        return ListChildren(childStart, childEnd, args.at(var_index), print_values, pThread, pFrame, output);
    }},
    { "var-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 1)
        {
            output = "Command requires at least 1 argument";
            return E_FAIL;
        }
        return DeleteVar(args.at(0));
    }},
    { "gdb-exit", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        this->m_exit = true;

        this->TerminateProcess();

        return S_OK;
    }},
    { "file-exec-and-symbols", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        m_fileExec = args.at(0);
        return S_OK;
    }},
    { "exec-arguments", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        m_execArgs = args;
        return S_OK;
    }},
    { "exec-run", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return RunProcess(); // TODO return ~running
    }},
    { "handshake", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!args.empty() && args.at(0) == "init")
            output = "request=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"";

        return S_OK;
    }},
    { "gdb-set", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() == 2)
        {
            if (args.at(0) == "just-my-code")
            {
                Debugger::SetJustMyCode(args.at(1) == "1");
            }
        }
        return S_OK;
    }},
    { "break-exception-insert", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_FAIL;
        size_t i = 1;
        if (args.at(0) == "--mda")
            i = 2;

        std::stringstream ss;
        const char *sep = "";
        ss << "bkpt=[";
        for (; i < args.size(); i++)
        {
            ULONG32 id = InsertExceptionBreakpoint(args.at(i));
            ss << sep;
            sep = ",";
            ss << "{number=\"" << id << "\"}";
        }
        ss << "]";
        output = ss.str();

        return S_OK;
    }},
    { "var-show-attributes", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        output = "status=\"noneditable\"";
        return S_OK;
    }},
    };

    auto command_it = commands.find(command);

    if (command_it == commands.end())
    {
        output = "Unknown command: " + command;
        return E_FAIL;
    }

    return command_it->second(m_pProcess, args, output);
}

static std::vector<std::string> TokenizeString(const std::string &str)
{
    enum {
        StateSpace,
        StateToken,
        StateQuotedToken,
        StateEscape
    } state = StateSpace;

    std::vector<std::string> result;

    static const char *delimiters = " \t\n\r";

    for (char c : str)
    {
        switch(state)
        {
            case StateSpace:
                if (strchr(delimiters, c))
                    continue;
                result.emplace_back();
                state = c == '"' ? StateQuotedToken : StateToken;
                if (state != StateQuotedToken)
                    result.back() +=c;
                break;
            case StateToken:
                if (strchr(delimiters, c))
                    state = StateSpace;
                else
                    result.back() += c;
                break;
            case StateQuotedToken:
                if (c == '\\')
                    state = StateEscape;
                else if (c == '"')
                    state = StateSpace;
                else
                    result.back() += c;
                break;
            case StateEscape:
                result.back() += c;
                state = StateQuotedToken;
                break;
        }
    }
    return result;
}

static bool ParseLine(const std::string &str,
                      std::string &token,
                      std::string &cmd,
                      std::vector<std::string> &args)
{
    token.clear();
    cmd.clear();
    args.clear();

    std::vector<std::string> result = TokenizeString(str);

    if (result.empty())
        return false;

    auto cmd_it = result.begin();

    std::size_t i = cmd_it->find_first_not_of("0123456789");
    if (i == std::string::npos)
        return false;

    if (cmd_it->at(i) != '-')
        return false;

    token = cmd_it->substr(0, i);
    cmd = cmd_it->substr(i + 1);
    result.erase(result.begin());
    args = result;

    return true;
}

void Debugger::CommandLoop()
{
    static char inputBuffer[1024];
    std::string token;

    while (!m_exit)
    {
        token.clear();

        Printf("(gdb)\n");
        if (!fgets(inputBuffer, _countof(inputBuffer), stdin))
            break;

        std::vector<std::string> args;
        std::string command;
        if (!ParseLine(inputBuffer, token, command, args))
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
        TerminateProcess();

    Printf("%s^exit\n", token.c_str());
}
