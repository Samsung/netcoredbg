// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cor.h>
#include <cordebug.h>
#pragma GCC diagnostic pop

#include <string>

HRESULT PrintValue(ICorDebugValue *pInputValue, std::string &output, bool escape = true);
HRESULT PrintBasicValue(int typeId, const std::string &rawData, std::string &typeName, std::string &value);
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = nullptr);
HRESULT MarshalValue(ICorDebugValue *pInputValue, int *typeId, void **data);
HRESULT PrintStringField(ICorDebugValue *pValue, const WCHAR *fieldName, std::string &output, ICorDebugType *pType = nullptr);
