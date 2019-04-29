// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "platform.h"

// From https://github.com/Microsoft/vscode-debugadapter-node/blob/master/protocol/src/debugProtocol.ts

struct Thread
{
    int id;
    std::string name;
    bool running;

    Thread(int id, std::string name, bool running) : id(id), name(name), running(running) {}
};

struct Source
{
    std::string name;
    std::string path;

    Source(std::string path = std::string()) : name(GetFileName(path)), path(path) {}
    bool IsNull() const { return name.empty() && path.empty(); }
};

struct ClrAddr
{
    uint32_t ilOffset;
    uint32_t nativeOffset;
    uint32_t methodToken;

    ClrAddr() : ilOffset(0), nativeOffset(0), methodToken(0) {}
    bool IsNull() const { return methodToken == 0; }
};

struct StackFrameData
{
    static std::unordered_map<uint64_t, uint32_t> idStore;
    static uint32_t nextId;
    int id;

    StackFrameData(int threadId, uint32_t level)
    {
        uint64_t key = threadId;
        key <<= 32;
        key |= level;
        if (idStore.find(key) == idStore.end())
        {
            StackFrameData::nextId++;
            idStore[key] = StackFrameData::nextId;
        }

        id = idStore[key];
    }
};

struct StackFrame
{
    uint32_t id;
    std::string name;
    Source source;
    int line;
    int column;
    int endLine;
    int endColumn;
    std::string moduleId;

    ClrAddr clrAddr; // exposed for MI protocol
    uint64_t addr; // exposed for MI protocol

    int threadId;
    uint32_t level;

    StackFrame() :
        id(0), line(0), column(0), endLine(0), endColumn(0), addr(0) {}

    StackFrame(int threadId, uint32_t level, std::string name) :
        name(name), line(0), column(0), endLine(0), endColumn(0), addr(0), threadId(threadId), level(level)
    {
        StackFrameData data = StackFrameData(threadId, level);
        id = data.id;
    }

    StackFrame(uint32_t id) : id(id), line(0), column(0), endLine(0), endColumn(0), addr(0) {}

    uint32_t GetLevel() const { return level; }
    int GetThreadId() const { return threadId; }
};

struct Breakpoint
{
    uint32_t id;
    bool verified;
    std::string message;
    Source source;
    int line;

    uint32_t hitCount; // exposed for MI protocol
    std::string condition;
    std::string module;
    std::string funcname;
    std::string params;

    Breakpoint() : id(0), verified(false), line(0), hitCount(0) {}
};

enum SymbolStatus
{
    SymbolsSkipped, // "Skipped loading symbols."
    SymbolsLoaded,  // "Symbols loaded." 
    SymbolsNotFound
};

struct Module
{
    std::string id;
    std::string name;
    std::string path;
    // bool isOptimized; // TODO: support both fields for VSCode protocol
    // bool isUserCode;
    SymbolStatus symbolStatus;
    uint64_t baseAddress; // exposed for MI protocol
    uint32_t size; // exposed for MI protocol

    Module() : symbolStatus(SymbolsSkipped), baseAddress(0), size(0) {}
};

enum BreakpointReason
{
    BreakpointChanged,
    BreakpointNew,
    BreakpointRemoved
};

enum StopReason
{
    StopStep,
    StopBreakpoint,
    StopException,
    StopPause,
    StopEntry
};

struct StoppedEvent
{
    StopReason reason;
    std::string description;
    int threadId;
    std::string text;
    bool allThreadsStopped;

    StackFrame frame; // exposed for MI protocol
    Breakpoint breakpoint; // exposed for MI protocol

    StoppedEvent(StopReason reason, int threadId = 0) : reason(reason), threadId(threadId), allThreadsStopped(true) {}
};

struct BreakpointEvent
{
    BreakpointReason reason;
    Breakpoint breakpoint;

    BreakpointEvent(BreakpointReason reason, Breakpoint breakpoint) : reason(reason), breakpoint(breakpoint) {}
};

struct ExitedEvent
{
    int exitCode;

    ExitedEvent(int exitCode) : exitCode(exitCode) {}
};

enum ThreadReason
{
    ThreadStarted,
    ThreadExited
};

struct ThreadEvent
{
    ThreadReason reason;
    int threadId;

    ThreadEvent(ThreadReason reason, int threadId) : reason(reason), threadId(threadId) {}
};

enum OutputCategory
{
    OutputConsole,
    OutputStdOut,
    OutputStdErr
};

struct OutputEvent
{
    OutputCategory category;
    std::string output;

    std::string source; // exposed for MI protocol

    OutputEvent(OutputCategory category, std::string output) : category(category), output(output) {}
};

enum ModuleReason
{
    ModuleNew,
    ModuleChanged,
    ModuleRemoved
};

struct ModuleEvent
{
    ModuleReason reason;
    Module module;
    ModuleEvent(ModuleReason reason, const Module &module) : reason(reason), module(module) {}
};

struct Scope
{
    std::string name;
    uint32_t variablesReference;
    int namedVariables;
    int indexedVariables;
    bool expensive;

    Scope() : variablesReference(0), namedVariables(0), expensive(false) {}

    Scope(uint32_t variablesReference, const std::string &name, int namedVariables) :
        name(name),
        variablesReference(variablesReference),
        namedVariables(namedVariables),
        indexedVariables(0),
        expensive(false)
    {}
};


// TODO: Replace strings with enums
struct VariablePresentationHint
{
    std::string kind;
    std::vector<std::string> attributes;
    std::string visibility;
};

struct Variable
{
    std::string name;
    std::string value;
    std::string type;
    VariablePresentationHint presentationHint;
    std::string evaluateName;
    uint32_t variablesReference;
    int namedVariables;
    int indexedVariables;

    Variable() : variablesReference(0), namedVariables(0), indexedVariables(0) {}
};

enum VariablesFilter
{
    VariablesNamed,
    VariablesIndexed,
    VariablesBoth
};

struct SourceBreakpoint
{
    int line;
    std::string condition;

    SourceBreakpoint(int linenum, const std::string &cond = std::string()) : line(linenum), condition(cond) {}
};

struct FunctionBreakpoint
{
    std::string module;
    std::string func;
    std::string params;
    std::string condition;

    FunctionBreakpoint(const std::string &module,
                       const std::string &func,
                       const std::string &params,
                       const std::string &cond = std::string()) :
        module(module),
        func(func),
        params(params),
        condition(cond)
    {}
};
