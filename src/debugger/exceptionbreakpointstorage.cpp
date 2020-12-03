// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/exceptionbreakpointstorage.h"

#include <string>

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cor.h>
#pragma GCC diagnostic pop

namespace netcoredbg
{

HRESULT ExceptionBreakpointStorage::Insert(uint32_t id, const ExceptionBreakMode &mode, const std::string &name)
{
    HRESULT Status = S_OK;
    // vsdbg each time creates a new exception breakpoint id.
    // But, for "*" name, the last `id' silently are deleted by vsdbg.
    if (name.compare("*") == 0)
    {
        if (bp.current_asterix_id != 0)
        {
            // Silent remove for global filter
            Status = Delete(bp.current_asterix_id);
        }
        bp.current_asterix_id = id;
    }

    bp.exceptionBreakpoints.insert(std::make_pair(name, mode));
    bp.table[id] = name;

    return Status;
}

HRESULT ExceptionBreakpointStorage::Delete(uint32_t id)
{
    const auto it = bp.table.find(id);
    if (it == bp.table.end())
    {
        return E_FAIL;
    }
    const std::string name = it->second;
    if (name.compare("*") == 0)
    {
        bp.current_asterix_id = 0;
    }
    bp.exceptionBreakpoints.erase(name);
    bp.table.erase(id);

    return S_OK;
}

bool ExceptionBreakpointStorage::Match(int dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category) const
{
    // INFO: #pragma once - its a reason for this constants
    const int FIRST_CHANCE = 1;
    const int USER_FIRST_CHANCE = 2;
    const int CATCH_HANDLER_FOUND = 3;
    const int UNHANDLED = 4;

    bool unsupported = (dwEventType == FIRST_CHANCE || dwEventType == USER_FIRST_CHANCE);
    if (unsupported)
        return false;

    // Try to match exactly by name after check global name "*"
    // ExceptionBreakMode can be specialized by explicit filter.
    ExceptionBreakMode mode;
    GetExceptionBreakMode(mode, "*");
    GetExceptionBreakMode(mode, exceptionName);
    if (category == ExceptionBreakCategory::ANY || category == mode.category)
    {
        if (dwEventType == CATCH_HANDLER_FOUND)
        {
            if (mode.UserUnhandled())
            {
                // Expected user-applications exceptions from throw(), but get
                // explicit/implicit exception from `System.' clases.
                const std::string SystemPrefix = "System.";
                if (exceptionName.compare(0, SystemPrefix.size(), SystemPrefix) != 0)
                    return true;
            }
            if (mode.Throw())
                return true;
        }
        if (dwEventType == UNHANDLED)
        {
            if (mode.Unhandled())
                return true;
        }
    }

    return false;
}

HRESULT ExceptionBreakpointStorage::GetExceptionBreakMode(ExceptionBreakMode &out, const std::string &name) const
{
    auto p = bp.exceptionBreakpoints.equal_range(name);
    if (p.first == bp.exceptionBreakpoints.end())
    {
        return E_FAIL;
    }

    out.category = p.first->second.category;
    out.flags |= p.first->second.flags;
    ++p.first;
    while (p.first != p.second)
    {
        if (out.category == ExceptionBreakCategory::ANY ||
            out.category == p.first->second.category)
        {
            out.flags |= p.first->second.flags;
        }
        ++p.first;
    }

    return S_OK;
}

} // namespace netcoredbg
