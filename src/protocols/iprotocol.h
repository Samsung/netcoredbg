// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>
#include <vector>

#include "debugger/debugger.h"

enum BreakType
{
    LineBreak,
    FuncBreak,
    Error
};

struct LineBreak
{
    std::string filename;
    unsigned int linenum;
    std::string condition;
};

struct FuncBreak
{
    std::string module;
    std::string funcname;
    std::string params;
    std::string condition;
};

class IProtocol : public Protocol
{
public:
    IProtocol() : Protocol() {}

protected:
    int ParseInt(const std::string &s, bool &ok);
    void StripArgs(std::vector<std::string> &args);
    int GetIntArg(const std::vector<std::string> &args, const std::string& name, int defaultValue);
    bool GetIndices(const std::vector<std::string> &args, int &index1, int &index2);
    BreakType GetBreakpointType(const std::vector<std::string> &args);
    std::string GetConditionPrepareArgs(std::vector<std::string> &args);
    bool ParseBreakpoint(std::vector<std::string> &args, struct LineBreak &lb);
    bool ParseBreakpoint(std::vector<std::string> &args, struct FuncBreak &fb);
    static std::string AddrToString(uint64_t addr);
};
