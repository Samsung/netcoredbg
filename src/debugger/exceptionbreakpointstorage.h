// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "protocols/protocol.h"

namespace netcoredbg
{

struct ExceptionBreakpointStorage
{
private:
    // vsdbg not supported list of exception breakpoint command
    struct ExceptionBreakpoint {
        ExceptionBreakpoint() : current_asterix_id(0) {}
        std::unordered_map<uint32_t, std::string> table;
        // For global filter (*) we need to know last id
        uint32_t current_asterix_id;
        // for customers its will to come some difficult for matching.
        // For netcoredbg approach based on single unique name for each
        // next of user exception.
        //std::unordered_map<std::string, ExceptionBreakMode> exceptionBreakpoints;
        std::unordered_multimap<std::string, ExceptionBreakMode> exceptionBreakpoints;
    };

    ExceptionBreakpoint bp;

public:
    HRESULT Insert(uint32_t id, const ExceptionBreakMode &mode, const std::string &name);
    HRESULT Delete(uint32_t id);
    bool Match(int dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category) const;
    HRESULT GetExceptionBreakMode(ExceptionBreakMode &out, const std::string &name) const;

    ExceptionBreakpointStorage() = default;
    ExceptionBreakpointStorage(ExceptionBreakpointStorage &&that) = default;
    ExceptionBreakpointStorage(const ExceptionBreakpointStorage &that) = delete;
};

} // namespace netcoredbg
