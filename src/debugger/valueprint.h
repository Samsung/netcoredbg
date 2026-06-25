// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>

namespace netcoredbg
{

class EvalHelpers;
class Evaluator;

HRESULT PrintValue(ICorDebugValue *pInputValue, std::string &output, bool escape = true,
                   bool *wasFallback = nullptr);
HRESULT PrintObjectValue(ICorDebugValue *pInputValue, ICorDebugThread *pThread,
                         EvalHelpers *pEvalHelpers, Evaluator *pEvaluator,
                         int evalFlags, std::string &output, bool escape = true);
HRESULT GetNullableValue(ICorDebugValue *pValue, ICorDebugValue **ppValueValue, ICorDebugValue **ppHasValueValue);
HRESULT PrintNullableValue(ICorDebugValue *pValue, std::string &outTextValue);
HRESULT PrintStringValue(ICorDebugValue * pValue, std::string &output);
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = nullptr);

} // namespace netcoredbg
