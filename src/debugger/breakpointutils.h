// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>

namespace netcoredbg
{

class IDebugger;

namespace BreakpointUtils
{
    HRESULT IsSameFunctionBreakpoint(ICorDebugFunctionBreakpoint *pBreakpoint1, ICorDebugFunctionBreakpoint *pBreakpoint2);
    HRESULT IsEnableByCondition(const std::string &condition, IDebugger *debugger, ICorDebugThread *pThread);
    HRESULT SkipBreakpoint(ICorDebugModule *pModule, mdMethodDef methodToken, bool justMyCode);
}

} // namespace netcoredbg
