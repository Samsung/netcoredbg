// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "valuewrite.h"

#include <locale>
#include <codecvt>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <vector>

#include <arrayholder.h>

#include "torelease.h"
#include "utils/utf.h"
#include "metadata/typeprinter.h"
#include "managed/interop.h"

namespace netcoredbg
{

HRESULT WriteValue(
    ICorDebugValue *pValue,
    const std::string &value,
    ICorDebugThread *pThread,
    Evaluator &evaluator,
    std::string &errorText)
{
    HRESULT Status;

    ULONG32 size;
    IfFailRet(pValue->GetSize(&size));

    std::string data;

    CorElementType corType;
    IfFailRet(pValue->GetType(&corType));

    static const std::unordered_map<int, std::string> cor2name = {
        {ELEMENT_TYPE_BOOLEAN, "System.Boolean"},
        {ELEMENT_TYPE_U1,      "System.Byte"},
        {ELEMENT_TYPE_I1,      "System.SByte"},
        {ELEMENT_TYPE_CHAR,    "System.Char"},
        {ELEMENT_TYPE_R8,      "System.Double"},
        {ELEMENT_TYPE_R4,      "System.Single"},
        {ELEMENT_TYPE_I4,      "System.Int32"},
        {ELEMENT_TYPE_U4,      "System.UInt32"},
        {ELEMENT_TYPE_I8,      "System.Int64"},
        {ELEMENT_TYPE_U8,      "System.UInt64"},
        {ELEMENT_TYPE_I2,      "System.Int16"},
        {ELEMENT_TYPE_U2,      "System.UInt16"},
        {ELEMENT_TYPE_I,       "System.IntPtr"},
        {ELEMENT_TYPE_U,       "System.UIntPtr"}
    };
    auto renamed = cor2name.find(corType);
    if (renamed != cor2name.end())
    {
        IfFailRet(SymbolReader::ParseExpression(value, renamed->second, data, errorText));
    }
    else if (corType == ELEMENT_TYPE_STRING)
    {
        IfFailRet(SymbolReader::ParseExpression(value, "System.String", data, errorText));

        ToRelease<ICorDebugValue> pNewString;
        IfFailRet(evaluator.CreateString(pThread, data, &pNewString));

        // Switch object addresses
        ToRelease<ICorDebugReferenceValue> pRefNew;
        IfFailRet(pNewString->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefNew));
        ToRelease<ICorDebugReferenceValue> pRefOld;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefOld));

        CORDB_ADDRESS addr;
        IfFailRet(pRefNew->GetValue(&addr));
        IfFailRet(pRefOld->SetValue(addr));

        return S_OK;
    }
    else if (corType == ELEMENT_TYPE_VALUETYPE || corType == ELEMENT_TYPE_CLASS)
    {
        std::string typeName;
        TypePrinter::GetTypeOfValue(pValue, typeName);
        if (typeName != "decimal")
        {
            errorText = "Unable to set value of type '" + typeName + "'";
            return E_FAIL;
        }
        IfFailRet(SymbolReader::ParseExpression(value, "System.Decimal", data, errorText));
    }
    else
    {
        errorText = "Unable to set value";
        return E_FAIL;
    }

    if (size != data.size())
    {
        errorText = "Marshalling size mismatch: " + std::to_string(size) + " != " + std::to_string(data.size());
        return E_FAIL;
    }

    ToRelease<ICorDebugGenericValue> pGenValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &pGenValue));
    IfFailRet(pGenValue->SetValue((LPVOID) &data[0]));

    return S_OK;
}

} // namespace netcoredbg
