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
    bool isNull() const { return name.empty() && path.empty(); }
};

struct ClrAddr
{
    uint32_t ilOffset;
    uint32_t nativeOffset;
    uint32_t methodToken;

    ClrAddr() : ilOffset(0), nativeOffset(0), methodToken(0) {}
    bool isNull() const { return methodToken == 0; }
};

struct StackFrame
{
    uint64_t id; // frame start address
    std::string name;
    Source source;
    int line;
    int column;
    int endLine;
    int endColumn;
    std::string moduleId;

    ClrAddr clrAddr;

    StackFrame() :
        id(0), line(0), column(0), endLine(0), endColumn(0) {}

    StackFrame(uint64_t id, std::string name) : id(id), name(name) {}
};
