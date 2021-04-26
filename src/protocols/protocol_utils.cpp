// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <algorithm>
#include <iomanip>
#include "protocol_utils.h"

namespace netcoredbg
{

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

        if (args.size() < ncnt)
            return BreakType::Error;
    }

    std::size_t i = args.at(ncnt).rfind(':');

    // TODO: Spaces are avaliable at least for Func breakpoints!

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

bool ParseBreakpoint(std::vector<std::string> &args, struct LineBreak &lb)
{
    bool ok;
    lb.condition = GetConditionPrepareArgs(args);
    std::string prepString("");

    if (args.size() == 1)
        prepString = args.at(0);
    else
        for (auto &str : args)
            prepString += str;

    std::size_t i = prepString.find('!');

    if (i == std::string::npos)
    {
        lb.module.clear();
    }
    else
    {
        lb.module = std::string(prepString, 0, i);
        prepString.erase(0, i + 1);
    }

    i = prepString.rfind(':');

    lb.filename = prepString.substr(0, i);
    lb.linenum = ProtocolUtils::ParseInt(prepString.substr(i + 1), ok);

    return ok && lb.linenum > 0;
}

bool ParseBreakpoint(std::vector<std::string> &args, struct FuncBreak &fb)
{
    fb.condition = GetConditionPrepareArgs(args);
    std::string prepString("");

    if (args.size() == 1)
        prepString = args.at(0);
    else
        for (auto &str : args)
            prepString += str;

    std::size_t i = prepString.find('!');

    if (i == std::string::npos)
    {
        fb.module.clear();
    }
    else
    {
        fb.module = std::string(prepString, 0, i);
        prepString.erase(0, i + 1);
    }

    i = prepString.find('(');
    if (i != std::string::npos)
    {
        std::size_t closeBrace = prepString.find(')');

        fb.params = std::string(prepString, i, closeBrace - i + 1);
        prepString.erase(i, closeBrace);
    }
    else
    {
        fb.params.clear();
    }

    fb.funcname = prepString;

    return true;
}

std::string AddrToString(uint64_t addr)
{
    std::ostringstream ss;
    ss << "0x" << std::setw(2 * sizeof(void*)) << std::setfill('0') << std::hex << addr;
    return ss.str();
}

} // namespace ProtocolUtils

} // namespace netcoredbg
