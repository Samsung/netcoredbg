#include "common.h"

#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <algorithm>

// Varobj
HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output);
HRESULT CreateVar(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &varobjName, const std::string &expression, std::string &output);
HRESULT ListChildren(const std::string &name, int print_values, ICorDebugThread *pThread, ICorDebugFrame *pFrame, std::string &output);
HRESULT DeleteVar(const std::string &varobjName);

// Modules
HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range);

// Breakpoints
HRESULT DeleteBreakpoint(ULONG32 id);
HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);

void _out_printf(const char *fmt, ...)
    __attribute__((format (printf, 1, 2)));

#define out_printf(fmt, ...) _out_printf(fmt, ##__VA_ARGS__)

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
HRESULT DisableAllBreakpointsAndSteppers(ICorDebugProcess *pProcess);

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

bool ParseBreakpoint(const std::vector<std::string> &args, std::string &filename, unsigned int &linenum)
{
    if (args.empty())
        return false;

    std::size_t i = args.at(0).rfind(':');

    if (i == std::string::npos)
        return false;

    filename = args.at(0).substr(0, i);
    std::string slinenum = args.at(0).substr(i + 1);

    bool ok;
    linenum = ParseInt(slinenum, ok);
    return ok && linenum > 0;
}

HRESULT BreakInsertCommand(
    ICorDebugProcess *pProcess,
    const std::vector<std::string> &args,
    std::string &output)
{
    std::string filename;
    unsigned int linenum;
    ULONG32 id;
    if (ParseBreakpoint(args, filename, linenum)
        && SUCCEEDED(CreateBreakpointInProcess(pProcess, filename, linenum, id)))
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

HRESULT RunStep(ICorDebugThread *pThread, StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

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

static CommandCallback StepCommand(StepType stepType)
{
    return [&](ICorDebugProcess *pProcess,
               const std::vector<std::string> &,
               std::string &)->HRESULT
        {
            HRESULT Status;
            ToRelease<ICorDebugThread> pThread;
            DWORD threadId = GetLastStoppedThreadId();
            IfFailRet(pProcess->GetThread(threadId, &pThread));
            IfFailRet(RunStep(pThread, stepType));
            IfFailRet(pProcess->Continue(0));
            return S_OK;
        };
}

static HRESULT ThreadInfoCommand(ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &output)
{
    return PrintThreadsState(pProcess, output);
}

static bool g_exit = false;

static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", ThreadInfoCommand },
    { "exec-continue", [](ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &){ return pProcess->Continue(0); } },
    { "exec-interrupt", [](ICorDebugProcess *pProcess, const std::vector<std::string> &, std::string &){ return pProcess->Stop(0); } },
    { "break-insert", BreakInsertCommand },
    { "break-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &) -> HRESULT {
        for (const std::string &idStr : args)
        {
            ULONG32 id = std::stoul(idStr);
            DeleteBreakpoint(id);
        }
        return S_OK;
    } },
    { "exec-step", StepCommand(STEP_IN) },
    { "exec-next", StepCommand(STEP_OVER) },
    { "exec-finish", StepCommand(STEP_OUT) },
    { "stack-list-frames", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        // TODO: Add parsing frame lowFrame and highFrame
        HRESULT Status;
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));
        int lowFrame = 0;
        int highFrame = INT_MAX;
        IfFailRet(PrintFrames(pThread, output, lowFrame, highFrame));
        return S_OK;
    }},
    { "stack-list-variables", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        // TODO: Add parsing arguments --frame
        HRESULT Status;
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pThread->GetActiveFrame(&pFrame));

        IfFailRet(ListVariables(pFrame, output));

        return S_OK;
    }},
    { "var-create", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        // TODO: Add parsing arguments --frame
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pThread->GetActiveFrame(&pFrame));

        return CreateVar(pThread, pFrame, args.at(0), args.at(1), output);
    }},
    { "var-list-children", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        int print_values = 0;
        int var_index = 0;
        if (!args.empty())
        {
            if (args.at(0) == "1" || args.at(0) == "--all-values")
            {
                print_values = 1;
                var_index++;
            }
            else if (args.at(0) == "2" || args.at(0) == "--simple-values")
            {
                print_values = 2;
                var_index++;
            }
        }

        if (args.size() < (var_index + 1))
        {
            output = "Command requires an argument";
            return E_FAIL;
        }

        // TODO: Add parsing arguments --frame and children indices
        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pThread->GetActiveFrame(&pFrame));
        return ListChildren(args.at(var_index), print_values, pThread, pFrame, output);
    }},
    { "var-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 1)
        {
            output = "Command requires at least 1 argument";
            return E_FAIL;
        }
        return DeleteVar(args.at(0));
    }},
    { "gdb-exit", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        g_exit = true;
        IfFailRet(pProcess->Stop(0));

        DisableAllBreakpointsAndSteppers(pProcess);

        Status = pProcess->Terminate(0);

        WaitProcessExited();

        return Status;
    }}
};

static bool ParseLine(const std::string &str,
                      std::string &token,
                      std::string &cmd,
                      std::vector<std::string> &args)
{
    token.clear();
    cmd.clear();
    args.clear();

    std::stringstream ss(str);

    std::vector<std::string> result;
    std::string buf;

    if (!(ss >> cmd))
        return false;

    std::size_t i = cmd.find_first_not_of("0123456789");
    if (i == std::string::npos)
        return false;

    if (cmd.at(i) != '-')
        return false;

    token = cmd.substr(0, i);
    cmd = cmd.substr(i + 1);

    while (ss >> buf)
        args.push_back(buf);

    return true;
}

void CommandLoop(ICorDebugProcess *pProcess)
{
    static char inputBuffer[1024];
    std::string token;

    while (!g_exit)
    {
        token.clear();

        out_printf("(gdb)\n");
        if (!fgets(inputBuffer, _countof(inputBuffer), stdin))
            break;

        std::vector<std::string> args;
        std::string command;
        if (!ParseLine(inputBuffer, token, command, args))
        {
            out_printf("%s^error,msg=\"Failed to parse input\"\n", token.c_str());
            continue;
        }

        auto command_it = commands.find(command);

        if (command_it == commands.end())
        {
            out_printf("%s^error,msg=\"Unknown command: %s\"\n", token.c_str(), command.c_str());
            continue;
        }

        std::string output;
        HRESULT hr = command_it->second(pProcess, args, output);
        if (g_exit)
            break;
        if (SUCCEEDED(hr))
        {
            const char *sep = output.empty() ? "" : ",";
            out_printf("%s^done%s%s\n", token.c_str(), sep, output.c_str());
        }
        else
        {
            const char *sep = output.empty() ? "" : " ";
            out_printf("%s^error,msg=\"Error: 0x%08x%s%s\"\n", token.c_str(), hr, sep, output.c_str());
        }
    }
    if (SUCCEEDED(pProcess->Stop(0)))
    {
        DisableAllBreakpointsAndSteppers(pProcess);
        pProcess->Detach();
    }
    out_printf("%s^exit\n", token.c_str());
}
