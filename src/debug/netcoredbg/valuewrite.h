// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <string>

#include <cor.h>
#include <cordebug.h>
#include "manageddebugger.h"

HRESULT WriteValue(
    ICorDebugValue *pValue,
    const std::string &value,
    ICorDebugThread *pThread,
    Evaluator &evaluator,
    std::string &errorText);
