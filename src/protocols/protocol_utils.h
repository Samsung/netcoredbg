// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <iostream>
#include <string>
#include <vector>
#include "interfaces/iprotocol.h"

namespace netcoredbg
{

enum BreakType
{
    LineBreak,
    FuncBreak,
    Error
};

struct LineBreak
{
    std::string module;
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

class BreakpointsHandle
{
private:
    std::unordered_map<std::string, std::unordered_map<uint32_t, LineBreakpoint> > m_lineBreakpoints;
    std::unordered_map<uint32_t, FuncBreakpoint> m_funcBreakpoints;
    std::unordered_map<uint32_t, ExceptionBreakpoint> m_exceptionBreakpoints;

public:
    HRESULT SetLineBreakpoint(std::shared_ptr<IDebugger> &sharedDebugger, const std::string &module, const std::string &filename,
                              int linenum, const std::string &condition, Breakpoint &breakpoints);
    HRESULT SetFuncBreakpoint(std::shared_ptr<IDebugger> &sharedDebugger, const std::string &module, const std::string &funcname,
                              const std::string &params, const std::string &condition, Breakpoint &breakpoint);
    HRESULT SetExceptionBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, std::vector<ExceptionBreakpoint> &excBreakpoints,
                                    std::vector<Breakpoint> &breakpoints);
    HRESULT SetLineBreakpointCondition(std::shared_ptr<IDebugger> &sharedDebugger, uint32_t id, const std::string &condition);
    HRESULT SetFuncBreakpointCondition(std::shared_ptr<IDebugger> &sharedDebugger, uint32_t id, const std::string &condition);
    void DeleteLineBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids);
    void DeleteFuncBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids);
    void DeleteExceptionBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids);
    void Cleanup();
};

namespace ProtocolUtils
{
    int ParseInt(const std::string &s, bool &ok);
    void StripArgs(std::vector<std::string> &args);
    int GetIntArg(const std::vector<std::string> &args, const std::string& name, int defaultValue);
    bool FindAndEraseArg(std::vector<std::string> &args, const std::string &name);
    bool GetIndices(const std::vector<std::string> &args, int &index1, int &index2);
    BreakType GetBreakpointType(const std::vector<std::string> &args);
    std::string GetConditionPrepareArgs(std::vector<std::string> &args);
    bool ParseBreakpoint(std::vector<std::string> &args, struct LineBreak &lb);
    bool ParseBreakpoint(std::vector<std::string> &args, struct FuncBreak &fb);
    std::string AddrToString(uint64_t addr);

} // namespace ProtocolUtils

} // namespace netcoredbg
