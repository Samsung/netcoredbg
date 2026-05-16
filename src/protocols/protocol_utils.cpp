// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "cor.h"
#include "cordebug.h"

#include <sstream>
#include <algorithm>
#include <iomanip>
#include <unordered_map>
#include "protocol_utils.h"
#include "interfaces/idebugger.h"
#include "utils/torelease.h"

namespace netcoredbg
{

void BreakpointsHandle::Cleanup()
{
    m_lineBreakpoints.clear();
    m_funcBreakpoints.clear();
    m_exceptionBreakpoints.clear();
}

HRESULT BreakpointsHandle::UpdateLineBreakpoint(std::shared_ptr<IDebugger> &sharedDebugger, int id, int linenum, Breakpoint &breakpoint)
{
    for (auto &breakpointsInSource : m_lineBreakpoints)
    {
        for (auto &brk : breakpointsInSource.second)
        {
            if (brk.first != (unsigned)id)
                continue;

            brk.second.line = linenum;

            breakpoint.id = brk.first;
            breakpoint.verified = false;
            breakpoint.condition = brk.second.condition;
            breakpoint.logMessage = brk.second.logMessage;
            breakpoint.source = breakpointsInSource.first;
            breakpoint.line = brk.second.line;
            breakpoint.endLine = brk.second.line;
            breakpoint.hitCount = 0;

            return sharedDebugger->UpdateLineBreakpoint(id, linenum, breakpoint);
        }
    }

    return E_INVALIDARG;
}

HRESULT BreakpointsHandle::SetLineBreakpoint(std::shared_ptr<IDebugger> &sharedDebugger,
                                             const std::string &module, const std::string &filename, int linenum,
                                             const std::string &condition, const LogMessage &logMessage, Breakpoint &breakpoint)
{
    HRESULT Status;

    auto &breakpointsInSource = m_lineBreakpoints[filename];
    std::vector<LineBreakpoint> lineBreakpoints;
    lineBreakpoints.reserve(breakpointsInSource.size() + 1); // size + new element
    for (auto it : breakpointsInSource)
        lineBreakpoints.push_back(it.second);

    lineBreakpoints.emplace_back(module, linenum, condition, logMessage);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(sharedDebugger->SetLineBreakpoints(filename, lineBreakpoints, breakpoints));

    // Note, SetLineBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "lineBreakpoints".
    breakpoint = breakpoints.back();
    breakpointsInSource.insert(std::make_pair(breakpoint.id, std::move(lineBreakpoints.back())));
    return S_OK;
}

HRESULT BreakpointsHandle::SetFuncBreakpoint(std::shared_ptr<IDebugger> &sharedDebugger,
                                             const std::string &module, const std::string &funcname, const std::string &params,
                                             const std::string &condition, const LogMessage &logMessage, Breakpoint &breakpoint)
{
    HRESULT Status;

    std::vector<FuncBreakpoint> funcBreakpoints;
    funcBreakpoints.reserve(m_funcBreakpoints.size() + 1); // size + new element
    for (const auto &it : m_funcBreakpoints)
        funcBreakpoints.push_back(it.second);

    funcBreakpoints.emplace_back(module, funcname, params, condition, logMessage);

    std::vector<Breakpoint> breakpoints;
    IfFailRet(sharedDebugger->SetFuncBreakpoints(funcBreakpoints, breakpoints));

    // Note, SetFuncBreakpoints() will return new breakpoint in "breakpoints" with same index as we have it in "funcBreakpoints".
    breakpoint = breakpoints.back();
    m_funcBreakpoints.insert(std::make_pair(breakpoint.id, std::move(funcBreakpoints.back())));
    return S_OK;
}

// Note, exceptionBreakpoints data will be invalidated by this call.
HRESULT BreakpointsHandle::SetExceptionBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger,
                                                   /* [in] */ std::vector<ExceptionBreakpoint> &exceptionBreakpoints,
                                                   /* [out] */ std::vector<Breakpoint> &breakpoints)
{
    HRESULT Status;

    std::vector<ExceptionBreakpoint> excBreakpoints;
    excBreakpoints.reserve(m_exceptionBreakpoints.size() + exceptionBreakpoints.size()); // size + new elements size
    for (const auto &it : m_exceptionBreakpoints)
        excBreakpoints.push_back(it.second);

    excBreakpoints.insert(excBreakpoints.end(), // Don't copy, but move exceptionBreakpoints into excBreakpoints.
                          std::make_move_iterator(exceptionBreakpoints.begin()), std::make_move_iterator(exceptionBreakpoints.end()));

    IfFailRet(sharedDebugger->SetExceptionBreakpoints(excBreakpoints, breakpoints));

    for (size_t i = m_exceptionBreakpoints.size(); i < breakpoints.size(); ++i)
        m_exceptionBreakpoints.insert(std::make_pair(breakpoints[i].id, std::move(excBreakpoints[i])));

    return S_OK;
}

HRESULT BreakpointsHandle::SetLineBreakpointCondition(std::shared_ptr<IDebugger> &sharedDebugger, uint32_t id, const std::string &condition)
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
        return sharedDebugger->SetLineBreakpoints(filename, existingBreakpoints, tmpBreakpoints);
    }

    return E_FAIL;
}

HRESULT BreakpointsHandle::SetFuncBreakpointCondition(std::shared_ptr<IDebugger> &sharedDebugger, uint32_t id, const std::string &condition)
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
    return sharedDebugger->SetFuncBreakpoints(existingFuncBreakpoints, tmpBreakpoints);
}

void BreakpointsHandle::DeleteLineBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids)
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
        sharedDebugger->SetLineBreakpoints(filename, remainingBreakpoints, tmpBreakpoints);
    }
}

void BreakpointsHandle::DeleteFuncBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids)
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
    sharedDebugger->SetFuncBreakpoints(remainingFuncBreakpoints, tmpBreakpoints);
}

void BreakpointsHandle::DeleteExceptionBreakpoints(std::shared_ptr<IDebugger> &sharedDebugger, const std::unordered_set<uint32_t> &ids)
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
    sharedDebugger->SetExceptionBreakpoints(remainingExceptionBreakpoints, tmpBreakpoints);
}

namespace ProtocolUtils
{

int ParseInt(const std::string &s, bool &ok)
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
void StripArgs(std::vector<std::string> &args)
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

int GetIntArg(const std::vector<std::string> &args, const std::string& name, int defaultValue)
{
    auto it = std::find(args.begin(), args.end(), name);

    if (it == args.end())
        return defaultValue;

    ++it;

    if (it == args.end())
        return defaultValue;

    bool ok;
    int val = ProtocolUtils::ParseInt(*it, ok);
    return ok ? val : defaultValue;
}

// Return `true` in case arg was found and erased.
bool FindAndEraseArg(std::vector<std::string> &args, const std::string& name)
{
    auto it = args.begin();
    while (it != args.end())
    {
        if (*it == name)
        {
            it = args.erase(it);
            return true;
        }
        else
            ++it;
    }
    return false;
}

bool GetIndices(const std::vector<std::string> &args, int &index1, int &index2)
{
    if (args.size() < 2)
        return false;

    bool ok;
    int val1 = ProtocolUtils::ParseInt(args.at(args.size() - 2), ok);
    if (!ok)
        return false;
    int val2 = ParseInt(args.at(args.size() - 1), ok);
    if (!ok)
        return false;
    index1 = val1;
    index2 = val2;
    return true;
}

BreakType GetBreakpointType(const std::vector<std::string> &args)
{
    unsigned ncnt = 0;

    if (args.empty())
        return BreakType::Error;

    if (args.at(0) == "-f")
    {
        ++ncnt;
        if (args.size() <= ncnt)
            return BreakType::Error;
    }

    if (args.at(ncnt) == "-c")
    {
        ncnt += 2;

        if (args.size() <= ncnt)
            return BreakType::Error;
    }

    std::size_t i = args.at(ncnt).rfind(':');

    // TODO: Spaces are available at least for Func breakpoints!

    if (i == std::string::npos)
        return BreakType::FuncBreak;

    // i + 1 to skip colon
    std::string linenum(args.at(ncnt), i + 1);

    if (linenum.find_first_not_of("0123456789") == std::string::npos)
        return BreakType::LineBreak;

    return BreakType::Error;
}

std::string GetConditionPrepareArgs(std::vector<std::string> &args)
{
    std::string condition("");

    if (args.at(0) == "-f")
        args.erase(args.begin());

    if (args.at(0) == "-c")
    {
        condition = args.at(1);
        args.erase(args.begin(), args.begin() + 2);
    }

    return condition;
}

bool CreateLogMessageFormat(const std::string &formatStr, std::vector<std::string> &format)
{
    std::size_t lastPos = 0, pos = 0;

    while ((pos = formatStr.find("%s", lastPos)) != std::string::npos)
    {
        format.push_back(formatStr.substr(lastPos, pos - lastPos));
        format.push_back("");
        lastPos = pos + 2;
    }

    if (lastPos < formatStr.size())
        format.push_back(formatStr.substr(lastPos));

    return true;
}

bool ParseLogMessage(const std::vector<std::string> &args, LogMessage &logMessage)
{
    if (args.empty() || !CreateLogMessageFormat(args[0], logMessage.format))
        return false;

    logMessage.args = std::vector<std::string>(args.begin() + 1, args.end());
    return true;
}

bool ParseBreakpoint(std::vector<std::string> &args, struct LineBreak &lb, bool isLogPoint)
{
    bool ok = false;
    lb.condition = GetConditionPrepareArgs(args);

    for (auto i = args.begin(); i != args.end(); ++i)
    {
        auto &arg = *i;

        std::size_t colonIndex = arg.rfind(':');

        if (colonIndex == std::string::npos)
            continue;

        auto moduleAndFile = arg.substr(0, colonIndex);

        std::size_t exclIndex = moduleAndFile.find('!');

        if (exclIndex != std::string::npos)
        {
            lb.module = moduleAndFile.substr(0, exclIndex);
            lb.filename = moduleAndFile.substr(exclIndex + 1);
        }
        else
        {
            lb.module.clear();
            lb.filename = moduleAndFile;
        }

        lb.linenum = ProtocolUtils::ParseInt(arg.substr(colonIndex + 1), ok);

        if (!ok)
        {
            lb.filename.clear();
            lb.linenum = 0;
            continue;
        }

        args.erase(args.begin(), i + 1);
        ok = true;
        break;
    }

    return ok && (!isLogPoint || ParseLogMessage(args, lb.logMessage));
}

bool ParseBreakpoint(std::vector<std::string> &args, struct FuncBreak &fb, bool isLogPoint)
{
    fb.condition = GetConditionPrepareArgs(args);

    auto i = args.begin();

    for (; i != args.end(); ++i)
    {
        auto &arg = *i;

        if (arg[0] == '-')
        {
            if (arg.size() >= 2 && arg[1] == '-' && ++i == args.end())
                return false;
        }
        else
            break;
    }

    if (i == args.end())
        return false;

    args.erase(args.begin(), i);

    auto &loc = args[0];

    std::size_t exclIndex = loc.find('!');

    if (exclIndex == std::string::npos)
        fb.module.clear();
    else
    {
        fb.module = std::string(loc, 0, exclIndex);
        loc.erase(0, exclIndex + 1);
    }

    auto parenIndex = loc.find('(');
    if (parenIndex != std::string::npos)
    {
        std::size_t closeParenIndex = loc.find(')', parenIndex);

        if (closeParenIndex == std::string::npos)
            return false;

        fb.params = std::string(loc, parenIndex, closeParenIndex - parenIndex + 1);
        loc.erase(parenIndex, closeParenIndex - parenIndex + 1);
    }
    else
        fb.params.clear();

    fb.funcname = loc;

    args.erase(args.begin());

    return !isLogPoint || ParseLogMessage(args, fb.logMessage);
}

std::string AddrToString(std::uintptr_t addr)
{
    std::ostringstream ss;
    ss << "0x" << std::setw(2 * sizeof(std::uintptr_t)) << std::setfill('0') << std::hex << addr;
    return ss.str();
}

} // namespace ProtocolUtils

} // namespace netcoredbg
