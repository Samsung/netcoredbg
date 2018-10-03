// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <cor.h>
#include <cordebug.h>

#include <string>

HRESULT PrintValue(ICorDebugValue *pInputValue, std::string &output, bool escape = true);
HRESULT PrintBasicValue(int typeId, const std::string &rawData, std::string &typeName, std::string &value);
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = nullptr);
HRESULT MarshalValue(ICorDebugValue *pInputValue, int *typeId, void **data);
