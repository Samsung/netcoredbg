// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cor.h>
#include <cordebug.h>
#pragma GCC diagnostic pop

#include "debugger/manageddebugger.h"

namespace netcoredbg
{

HRESULT WriteValue(
    ICorDebugValue *pValue,
    const std::string &value,
    ICorDebugThread *pThread,
    Evaluator &evaluator,
    std::string &errorText);

} // namespace netcoredbg
