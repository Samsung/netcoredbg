// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "platform.h"
#include <string>

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

struct StackFrame
{
    uint64_t id; // (threadId << 32) | level
    std::string name;
    Source source;
    int line;
    int column;
    int endLine;
    int endColumn;
    std::string moduleId;

    ClrAddr clrAddr; // exposed for MI protocol
    uint64_t addr; // exposed for MI protocol

    StackFrame() :
        id(0), line(0), column(0), endLine(0), endColumn(0) {}

    StackFrame(int threadId, uint32_t level, std::string name) : name(name)
    {
        id = threadId;
        id <<= 32;
        id |= level;
    }

    uint32_t GetLevel() const { return id & 0xFFFFFFFFul; }
    int GetThreadId() const { return id >> 32; }
};

struct Breakpoint
{
    uint32_t id;
    bool verified;
    std::string message;
    Source source;
    int line;

    uint32_t hitCount; // exposed for MI protocol

    Breakpoint() : id(0), verified(false), line(0), hitCount(0) {}
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

    OutputEvent(OutputCategory category, std::string output) : category(OutputConsole), output(output) {}
};
