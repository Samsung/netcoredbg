// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <memory>
#include <unordered_set>
#include <vector>
#include "debugger/evalhelpers.h"
#include "debugger/evalutils.h"
#include "debugger/evaluator.h"
#include "debugger/frames.h"
#include "utils/utf.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "valueprint.h"
#include "managed/interop.h"

namespace netcoredbg
{

static HRESULT ParseIndices(const std::string &s, std::vector<ULONG32> &indices)
{
    indices.clear();
    ULONG32 currentVal = 0;
    bool digit_detected = false;

    static const std::string digits("0123456789");

    for (char c : s)
    {
        std::size_t digit = digits.find(c);
        if (digit == std::string::npos)
        {
            switch(c)
            {
                case ' ': continue;
                case ',':
                case ']':
                    if (!digit_detected)
                        return E_FAIL;
                    indices.push_back(currentVal);
                    if (c == ']')
                        return S_OK;
                    currentVal = 0;
                    digit_detected = false;
                    continue;
                default:
                    return E_FAIL;
            }
        }
        currentVal *= 10;
        currentVal += (ULONG32)digit;
        digit_detected = true;
    }

    // Something wrong, we should never arrived this point.
    return E_FAIL;
}

HRESULT Evaluator::GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                              FrameLevel frameLevel,
                                              ICorDebugValue *pInputValue,
                                              ValueKind valueKind,
                                              const std::string &name,
                                              ICorDebugValue **ppResultValue,
                                              int evalFlags)
{
    HRESULT Status;

    if (name.empty())
        return E_FAIL;

    if (name.back() == ']')
    {
        if (valueKind == ValueIsClass)
            return E_FAIL;

        BOOL isNull = FALSE;
        ToRelease<ICorDebugValue> pValue;

        IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

        if (isNull)
            return E_FAIL;

        ToRelease<ICorDebugArrayValue> pArrayVal;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayVal));

        ULONG32 nRank;
        IfFailRet(pArrayVal->GetRank(&nRank));

        std::vector<ULONG32> indices;
        IfFailRet(ParseIndices(name, indices));

        if (indices.size() != nRank)
            return E_FAIL;

        ToRelease<ICorDebugValue> pArrayElement;
        return pArrayVal->GetElement(static_cast<uint32_t>(indices.size()), indices.data(), ppResultValue);
    }

    WalkMembers(pInputValue, pThread, frameLevel, [&](
        ICorDebugType *pType,
        bool is_static,
        const std::string &memberName,
        GetValueCallback getValue,
        SetValueCallback)
    {
        if (is_static && valueKind == ValueIsVariable)
            return S_OK;
        if (!is_static && valueKind == ValueIsClass)
            return S_OK;

        if (memberName != name)
            return S_OK;

        IfFailRet(getValue(ppResultValue, evalFlags));

        return E_ABORT; // Fast exit from cycle with result.
    });

    return *ppResultValue != nullptr ? S_OK : E_FAIL;
}

HRESULT Evaluator::FollowFields(ICorDebugThread *pThread,
                                FrameLevel frameLevel,
                                ICorDebugValue *pValue,
                                ValueKind valueKind,
                                const std::vector<std::string> &parts,
                                int nextPart,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    // Note, in case of (nextPart == parts.size()) result is pValue itself, so, we ok here.
    if (nextPart > (int)parts.size())
        return E_FAIL;

    pValue->AddRef();
    ToRelease<ICorDebugValue> pResultValue(pValue);
    for (int i = nextPart; i < (int)parts.size(); i++)
    {
        ToRelease<ICorDebugValue> pClassValue(std::move(pResultValue));
        IfFailRet(GetFieldOrPropertyWithName(
            pThread, frameLevel, pClassValue, valueKind, parts[i], &pResultValue, evalFlags));  // NOLINT(clang-analyzer-cplusplus.Move)
        valueKind = ValueIsVariable; // we can only follow through instance fields
    }

    *ppResult = pResultValue.Detach();
    return S_OK;
}

static std::vector<std::string> ParseExpression(const std::string &expression)
{
    std::vector<std::string> result;
    int paramDepth = 0;

    result.push_back("");

    for (char c : expression)
    {
        switch(c)
        {
            case '.':
            case '[':
                if (paramDepth == 0)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '<':
                paramDepth++;
                break;
            case '>':
                paramDepth--;
                break;
            case ' ':
                continue;
            default:
                break;
        }
        result.back() += c;
    }
    return result;
}

HRESULT Evaluator::FollowNested(ICorDebugThread *pThread,
                                FrameLevel frameLevel,
                                const std::string &methodClass,
                                const std::vector<std::string> &parts,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    std::vector<int> ranks;
    std::vector<std::string> classParts = EvalUtils::ParseType(methodClass, ranks);
    int nextClassPart = 0;
    int partsNum = (int)parts.size() -1;
    std::vector<std::string> fieldName {parts.back()};
    std::vector<std::string> fullpath;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(EvalUtils::FindType(classParts, nextClassPart, pThread, m_sharedModules.get(), nullptr, nullptr, &pModule));

    bool trim = false;
    while (!classParts.empty())
    {
        ToRelease<ICorDebugType> pType;
        nextClassPart = 0;
        if (trim)
            classParts.pop_back();

        fullpath = classParts;
        for (int i = 0; i < partsNum; i++)
            fullpath.push_back(parts[i]);

        if (FAILED(EvalUtils::FindType(fullpath, nextClassPart, pThread, m_sharedModules.get(), pModule, &pType)))  // NOLINT(clang-analyzer-cplusplus.Move)
            break;

        if(nextClassPart < (int)fullpath.size())
        {
            // try to check non-static fields inside a static member
            std::vector<std::string> staticName;
            for (int i = nextClassPart; i < (int)fullpath.size(); i++)
            {
                staticName.push_back(fullpath[i]);
            }
            staticName.push_back(fieldName[0]);
            ToRelease<ICorDebugValue> pTypeObject;
            if (S_OK == m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject))
            {
                if (SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, staticName, 0, ppResult, evalFlags)))
                    return S_OK;
            }
            trim = true;
            continue;
        }

        ToRelease<ICorDebugValue> pTypeObject;
        IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject));
        if (Status == S_OK && // type have static members (S_FALSE if type don't have static members)
            SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, fieldName, 0, ppResult, evalFlags)))
            return S_OK;

        trim = true;
    }

    return E_FAIL;
}

HRESULT Evaluator::EvalExpr(ICorDebugThread *pThread,
                            FrameLevel frameLevel,
                            const std::string &expression,
                            ICorDebugValue **ppResult,
                            int evalFlags)
{
    HRESULT Status;

    // TODO: support generics
    std::vector<std::string> parts = ParseExpression(expression);

    if (parts.empty())
        return E_FAIL;

    int nextPart = 0;

    ToRelease<ICorDebugValue> pResultValue;
    ToRelease<ICorDebugValue> pThisValue;

    if (parts.at(nextPart) == "$exception")
    {
        IfFailRet(pThread->GetCurrentException(&pResultValue));
        if (pResultValue == nullptr)
            return E_FAIL;
    }
    else
    {
        // Note, we use E_ABORT error code as fast way to exit from stack vars walk routine here.
        if (FAILED(Status = WalkStackVars(pThread, frameLevel, [&](const std::string &name,
                                                                   GetValueCallback getValue) -> HRESULT
        {
            if (name == "this")
            {
                if (FAILED(getValue(&pThisValue, evalFlags)) || !pThisValue)
                    return S_OK;

                if (name == parts.at(nextPart))
                    return E_ABORT; // Fast way to exit from stack vars walk routine.
            }
            else if (name == parts.at(nextPart))
            {
                if (FAILED(getValue(&pResultValue, evalFlags)) || !pResultValue)
                    return S_OK;

                return E_ABORT; // Fast way to exit from stack vars walk routine.
            }

            return S_OK;
        })) && !pThisValue && !pResultValue) // Check, that we have fast exit instead of real error.
        {
            return Status;
        }
    }

    if (!pResultValue && pThisValue) // check this/this.*
    {
        if (parts[nextPart] == "this")
            nextPart++; // skip first part with "this" (we have it in pThisValue), check rest

        if (SUCCEEDED(FollowFields(pThread, frameLevel, pThisValue, ValueIsVariable, parts, nextPart, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    if (!pResultValue) // check statics in nested classes
    {
        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
        if (pFrame == nullptr)
            return E_FAIL;

        std::string methodClass;
        std::string methodName;
        TypePrinter::GetTypeAndMethod(pFrame, methodClass, methodName);

        if (SUCCEEDED(FollowNested(pThread, frameLevel, methodClass, parts, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    ValueKind valueKind;
    if (pResultValue)
    {
        nextPart++;
        if (nextPart == (int)parts.size())
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
        valueKind = ValueIsVariable;
    }
    else
    {
        ToRelease<ICorDebugType> pType;
        IfFailRet(EvalUtils::FindType(parts, nextPart, pThread, m_sharedModules.get(), nullptr, &pType));
        IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pResultValue));
        if (Status == S_FALSE) // type don't have static members, nothing explore here
            return E_INVALIDARG;
        valueKind = ValueIsClass;
    }

    ToRelease<ICorDebugValue> pValue(std::move(pResultValue));
    IfFailRet(FollowFields(pThread, frameLevel, pValue, valueKind, parts, nextPart, &pResultValue, evalFlags));

    *ppResult = pResultValue.Detach();

    return S_OK;
}

static void IncIndicies(std::vector<ULONG32> &ind, const std::vector<ULONG32> &dims)
{
    int i = static_cast<int32_t>(ind.size()) - 1;

    while (i >= 0)
    {
        ind[i] += 1;
        if (ind[i] < dims[i])
            return;
        ind[i] = 0;
        --i;
    }
}

static std::string IndiciesToStr(const std::vector<ULONG32> &ind, const std::vector<ULONG32> &base)
{
    const size_t ind_size = ind.size();
    if (ind_size < 1 || base.size() != ind_size)
        return std::string();

    std::ostringstream ss;
    const char *sep = "";
    for (size_t i = 0; i < ind_size; ++i)
    {
        ss << sep;
        sep = ", ";
        ss << (base[i] + ind[i]);
    }
    return ss.str();
}
typedef std::function<HRESULT(mdFieldDef)> WalkFieldsCallback;
typedef std::function<HRESULT(mdProperty)> WalkPropertiesCallback;

static HRESULT ForEachFields(IMetaDataImport *pMD, mdTypeDef currentTypeDef, WalkFieldsCallback cb)
{
    HRESULT Status = S_OK;
    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&hEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        Status = cb(fieldDef);
        if (FAILED(Status))
            break;
    }
    pMD->CloseEnum(hEnum);
    return Status;
}

static HRESULT ForEachProperties(IMetaDataImport *pMD, mdTypeDef currentTypeDef, WalkPropertiesCallback cb)
{
    HRESULT Status = S_OK;
    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, currentTypeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        Status = cb(propertyDef);
        if (FAILED(Status))
            break;
    }
    pMD->CloseEnum(propEnum);
    return Status;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pInputValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    ICorDebugType *pTypeCast,
    WalkMembersCallback cb)
{
    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull && !pValue.GetPtr()) return S_OK;
    else if (!pValue.GetPtr()) return E_FAIL;

    CorElementType inputCorType;
    IfFailRet(pInputValue->GetType(&inputCorType));
    if (inputCorType == ELEMENT_TYPE_PTR)
    {
        auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
        {
            pValue->AddRef();
            *ppResultValue = pValue;
            return S_OK;
        };

        auto setValue = [&](const std::string &value, std::string &output, int evalFlags) -> HRESULT
        {
            if (!pThread)
                return E_FAIL;

            ToRelease<ICorDebugValue> iCorValue;
            IfFailRet(getValue(&iCorValue, evalFlags));
            return SetValue(iCorValue, value, pThread, output);
        };

        return cb(nullptr, false, "", getValue, setValue);
    }

    ToRelease<ICorDebugArrayValue> pArrayValue;
    if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayValue)))
    {
        ULONG32 nRank;
        IfFailRet(pArrayValue->GetRank(&nRank));

        ULONG32 cElements;
        IfFailRet(pArrayValue->GetCount(&cElements));

        std::vector<ULONG32> dims(nRank, 0);
        IfFailRet(pArrayValue->GetDimensions(nRank, &dims[0]));

        std::vector<ULONG32> base(nRank, 0);
        BOOL hasBaseIndicies = FALSE;
        if (SUCCEEDED(pArrayValue->HasBaseIndicies(&hasBaseIndicies)) && hasBaseIndicies)
            IfFailRet(pArrayValue->GetBaseIndicies(nRank, &base[0]));

        std::vector<ULONG32> ind(nRank, 0);

        for (ULONG32 i = 0; i < cElements; ++i)
        {
            auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
            {
                IfFailRet(pArrayValue->GetElementAtPosition(i, ppResultValue));
                return S_OK;
            };

            auto setValue = [&](const std::string &value, std::string &output, int evalFlags) -> HRESULT
            {
                if (!pThread)
                    return E_FAIL;

                ToRelease<ICorDebugValue> iCorValue;
                IfFailRet(getValue(&iCorValue, evalFlags));
                return SetValue(iCorValue, value, pThread, output);
            };

            IfFailRet(cb(nullptr, false, "[" + IndiciesToStr(ind, base) + "]", getValue, setValue));
            IncIndicies(ind, dims);
        }

        return S_OK;
    }

    ToRelease<ICorDebugValue2> pValue2;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    ToRelease<ICorDebugType> pType;
    if(pTypeCast == nullptr)
    {
        IfFailRet(pValue2->GetExactType(&pType));
        if (!pType) return E_FAIL;
    }
    else
    {
        pTypeCast->AddRef();
        pType = pTypeCast;
    }

    std::string className;
    TypePrinter::GetTypeOfValue(pType, className);
    if (className == "decimal") // TODO: implement mechanism for walking over custom type fields
        return S_OK;

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
        return S_OK;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    mdTypeDef currentTypeDef;
    IfFailRet(pClass->GetToken(&currentTypeDef));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));
    IfFailRet(ForEachFields(pMD, currentTypeDef, [&](mdFieldDef fieldDef) -> HRESULT
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        PCCOR_SIGNATURE pSignatureBlob = nullptr;
        ULONG sigBlobLength = 0;
        UVCP_CONSTANT pRawValue = nullptr;
        ULONG rawValueLength = 0;
        if (SUCCEEDED(pMD->GetFieldProps(fieldDef, nullptr, mdName, _countof(mdName), &nameLen, &fieldAttr,
                                         &pSignatureBlob, &sigBlobLength, nullptr, &pRawValue, &rawValueLength)))
        {
            // Prevent access to internal compiler added fields (without visible name).
            // Should be accessed by debugger routine only and hidden from user/ide.
            // More about compiler generated names in Roslyn sources:
            // https://github.com/dotnet/roslyn/blob/315c2e149ba7889b0937d872274c33fcbfe9af5f/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNames.cs
            // Note, uncontrolled access to internal compiler added field or its properties may break debugger work.
            if (nameLen > 2 && starts_with(mdName, W("<")))
                return S_OK;

            bool is_static = (fieldAttr & fdStatic);
            if (isNull && !is_static)
                return S_OK;

            std::string name = to_utf8(mdName);

            auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
            {
                if (fieldAttr & fdLiteral)
                {
                    IfFailRet(m_sharedEvalHelpers->GetLiteralValue(pThread, pType, pModule, pSignatureBlob, sigBlobLength, pRawValue, rawValueLength, ppResultValue));
                }
                else if (fieldAttr & fdStatic)
                {
                    if (!pThread)
                        return E_FAIL;

                    ToRelease<ICorDebugFrame> pFrame;
                    IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));

                    if (pFrame == nullptr)
                        return E_FAIL;

                    IfFailRet(pType->GetStaticFieldValue(fieldDef, pFrame, ppResultValue));
                }
                else
                {
                    // Get pValue again, since it could be neutered at eval call in `cb` on previous cycle.
                    pValue.Free();
                    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));
                    ToRelease<ICorDebugObjectValue> pObjValue;
                    IfFailRet(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue));
                    IfFailRet(pObjValue->GetFieldValue(pClass, fieldDef, ppResultValue));
                }

                return S_OK;
            };

            auto setValue = [&](const std::string &value, std::string &output, int evalFlags) -> HRESULT
            {
                if (!pThread)
                    return E_FAIL;

                ToRelease<ICorDebugValue> iCorValue;
                IfFailRet(getValue(&iCorValue, evalFlags));
                return SetValue(iCorValue, value, pThread, output);
            };

            IfFailRet(cb(pType, is_static, name, getValue, setValue));
        }
        return S_OK;
    }));
    IfFailRet(ForEachProperties(pMD, currentTypeDef, [&](mdProperty propertyDef) -> HRESULT
    {
        mdTypeDef  propertyClass;

        ULONG propertyNameLen = 0;
        UVCP_CONSTANT pDefaultValue;
        ULONG cchDefaultValue;
        mdMethodDef mdGetter;
        mdMethodDef mdSetter;
        WCHAR propertyName[mdNameLen] = W("\0");
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef, &propertyClass, propertyName, _countof(propertyName),
                                            &propertyNameLen, nullptr, nullptr, nullptr, nullptr, &pDefaultValue,
                                            &cchDefaultValue, &mdSetter, &mdGetter, nullptr, 0, nullptr)))
        {
            DWORD getterAttr = 0;
            if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
                return S_OK;

            bool is_static = (getterAttr & mdStatic);
            if (isNull && !is_static)
                return S_OK;

            // https://github.sec.samsung.net/dotnet/coreclr/blob/9df87a133b0f29f4932f38b7307c87d09ab80d5d/src/System.Private.CoreLib/shared/System/Diagnostics/DebuggerBrowsableAttribute.cs#L17
            // Since we check only first byte, no reason store it as int (default enum type in c#)
            enum DebuggerBrowsableState : char
            {
                Never = 0,
                Expanded = 1, 
                Collapsed = 2,
                RootHidden = 3
            };

            const char *g_DebuggerBrowsable = "System.Diagnostics.DebuggerBrowsableAttribute..ctor";
            bool debuggerBrowsableState_Never = false;

            ULONG numAttributes = 0;
            HCORENUM hEnum = NULL;
            mdCustomAttribute attr;
            while(SUCCEEDED(pMD->EnumCustomAttributes(&hEnum, propertyDef, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
            {
                mdToken ptkObj = mdTokenNil;
                mdToken ptkType = mdTokenNil;
                void const *ppBlob = 0;
                ULONG pcbSize = 0;
                if (FAILED(pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, &ppBlob, &pcbSize)))
                    continue;

                std::string mdName;
                if (FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, nullptr)))
                    continue;

                if (mdName == g_DebuggerBrowsable
                    // In case of DebuggerBrowsableAttribute blob is 8 bytes:
                    // 2 bytes - blob prolog 0x0001
                    // 4 bytes - data (DebuggerBrowsableAttribute::State), default enum type (int)
                    // 2 bytes - alignment
                    // We check only one byte (first data byte), no reason check 4 bytes in our case.
                    && pcbSize > 2
                    && ((char const *)ppBlob)[2] == DebuggerBrowsableState::Never)
                {
                    debuggerBrowsableState_Never = true;
                    break;
                }
            }
            pMD->CloseEnum(hEnum);

            if (debuggerBrowsableState_Never)
                return S_OK;

            std::string name = to_utf8(propertyName);

            auto getValue = [&](ICorDebugValue **ppResultValue, int evalFlags) -> HRESULT
            {
                if (!pThread)
                    return E_FAIL;

                ToRelease<ICorDebugFunction> iCorFunc;
                IfFailRet(pModule->GetFunctionFromToken(mdGetter, &iCorFunc));

                return m_sharedEvalHelpers->EvalFunction(pThread, iCorFunc, pType.GetRef(), 1, is_static ? nullptr : &pInputValue, is_static ? 0 : 1, ppResultValue, evalFlags);
            };

            auto setValue = [&](const std::string &value, std::string &output, int evalFlags) -> HRESULT
            {
                if (!pThread)
                    return E_FAIL;

                ToRelease<ICorDebugFunction> iCorFunc;
                IfFailRet(pModule->GetFunctionFromToken(mdSetter, &iCorFunc));

                // Find real type of property.
                ToRelease<ICorDebugValue> iCorValue;
                IfFailRet(getValue(&iCorValue, evalFlags));
                ToRelease<ICorDebugValue2> iCorValue2;
                IfFailRet(iCorValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
                ToRelease<ICorDebugType> iCorTmpType;
                IfFailRet(iCorValue2->GetExactType(&iCorTmpType));

                // Create temporary variable with value we need set via setter.
                CorElementType corType;
                IfFailRet(iCorValue->GetType(&corType));
                ToRelease<ICorDebugValue> iCorTmpValue;
                if (corType == ELEMENT_TYPE_STRING)
                {
                    std::string data;
                    IfFailRet(Interop::ParseExpression(value, "System.String", data, output));
                    IfFailRet(m_sharedEvalHelpers->CreateString(pThread, data, &iCorTmpValue));
                }
                else if (corType == ELEMENT_TYPE_ARRAY || corType == ELEMENT_TYPE_SZARRAY)
                {
                    // ICorDebugEval2::NewParameterizedArray
                    return E_NOTIMPL;
                }
                else // Allow SetValue() decide what types are supported.
                {
                    // https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/icordebugeval2-createvaluefortype-method
                    // ICorDebugEval2::CreateValueForType
                    // The type must be a class or a value type. You cannot use this method to create array values or string values.
                    ToRelease<ICorDebugEval> iCorEval;
                    IfFailRet(pThread->CreateEval(&iCorEval));
                    ToRelease<ICorDebugEval2> iCorEval2;
                    IfFailRet(iCorEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &iCorEval2));
                    IfFailRet(iCorEval2->CreateValueForType(iCorTmpType, &iCorTmpValue));
                    IfFailRet(SetValue(iCorTmpValue, value, pThread, output));
                }

                // Call setter.
                if (is_static)
                {
                    return m_sharedEvalHelpers->EvalFunction(pThread, iCorFunc, iCorTmpType.GetRef(), 1, iCorTmpValue.GetRef(), 1, nullptr, evalFlags);
                }
                else
                {
                    ICorDebugType *ppArgsType[] = {pType, iCorTmpType};
                    ICorDebugValue *ppArgsValue[] = {pInputValue, iCorTmpValue};
                    return m_sharedEvalHelpers->EvalFunction(pThread, iCorFunc, ppArgsType, 2, ppArgsValue, 2, nullptr, evalFlags);
                }
            };
            IfFailRet(cb(pType, is_static, name, getValue, setValue));
        }
        return S_OK;
    }));

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if (baseTypeName != "object" && baseTypeName != "System.Object" && baseTypeName != "System.ValueType")
        {
            if (pThread)
            {
                // Note, this call could return S_FALSE without ICorDebugValue creation in case type don't have static members.
                IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pBaseType));
            }
            // Add fields of base class
            IfFailRet(WalkMembers(pInputValue, pThread, frameLevel, pBaseType, cb));
        }
    }

    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    WalkMembersCallback cb)
{
    return WalkMembers(pValue, pThread, frameLevel, nullptr, cb);
}

static bool has_prefix(const std::string &s, const std::string &prefix)
{
    return prefix.length() <= s.length() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

HRESULT Evaluator::HandleSpecialLocalVar(
    const std::string &localName,
    ICorDebugValue *pLocalValue,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    // https://github.com/dotnet/roslyn/blob/315c2e149ba7889b0937d872274c33fcbfe9af5f/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNames.cs#L415
    static const std::string captureName = "CS$<>8__locals";

    HRESULT Status;

    if (!has_prefix(localName, captureName))
        return S_FALSE;

    // Substitute local value with its fields
    IfFailRet(WalkMembers(pLocalValue, nullptr, FrameLevel{0}, [&](
        ICorDebugType *,
        bool is_static,
        const std::string &name,
        GetValueCallback getValue,
        SetValueCallback)
    {
        if (is_static)
            return S_OK;

        if (has_prefix(name, captureName))
            return S_OK;

        if (!locals.insert(name).second)
            return S_OK; // already in the list

        return cb(name.empty() ? "this" : name, getValue);
    }));

    return S_OK;
}

HRESULT Evaluator::HandleSpecialThisParam(
    ICorDebugValue *pThisValue,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    static const std::string displayClass = "<>c__DisplayClass";

    HRESULT Status;

    std::string typeName;
    TypePrinter::GetTypeOfValue(pThisValue, typeName);

    std::size_t start = typeName.find_last_of('.');
    if (start == std::string::npos)
        return S_FALSE;

    typeName = typeName.substr(start + 1);

    if (!has_prefix(typeName, displayClass))
        return S_FALSE;

    // Substitute this with its fields
    IfFailRet(WalkMembers(pThisValue, nullptr, FrameLevel{0}, [&](
        ICorDebugType *,
        bool is_static,
        const std::string &name,
        GetValueCallback getValue,
        SetValueCallback)
    {
        HRESULT Status;
        if (is_static)
            return S_OK;

        ToRelease<ICorDebugValue> iCorResultValue;
        // We don't have properties here (even don't provide thread in WalkMembers), eval flag not in use.
        IfFailRet(getValue(&iCorResultValue, defaultEvalFlags));

        IfFailRet(HandleSpecialLocalVar(name, iCorResultValue, locals, cb));
        if (Status == S_OK)
            return S_OK;

        locals.insert(name);
        return cb(name.empty() ? "this" : name, getValue);
    }));
    return S_OK;
}

HRESULT Evaluator::WalkStackVars(ICorDebugThread *pThread, FrameLevel frameLevel, WalkStackVarsCallback cb)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunction->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdMethodDef methodDef;
    IfFailRet(pFunction->GetToken(&methodDef));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 currentIlOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&currentIlOffset, &mappingResult));

    ToRelease<ICorDebugValueEnum> pLocalsEnum;
    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));

    ULONG cLocals = 0;
    IfFailRet(pLocalsEnum->GetCount(&cLocals));

    ULONG cParams = 0;
    ToRelease<ICorDebugValueEnum> pParamEnum;
    IfFailRet(pILFrame->EnumerateArguments(&pParamEnum));
    IfFailRet(pParamEnum->GetCount(&cParams));

    std::unordered_set<std::string> locals;

    if (cParams > 0)
    {
        DWORD methodAttr = 0;
        IfFailRet(pMD->GetMethodProps(methodDef, NULL, NULL, 0, NULL, &methodAttr, NULL, NULL, NULL, NULL));

        for (ULONG i = 0; i < cParams; i++)
        {
            ULONG paramNameLen = 0;
            mdParamDef paramDef;
            std::string paramName;

            bool thisParam = i == 0 && (methodAttr & mdStatic) == 0;
            if (thisParam)
                paramName = "this";
            else
            {
                int idx = ((methodAttr & mdStatic) == 0)? i : (i + 1);
                if(SUCCEEDED(pMD->GetParamForMethodIndex(methodDef, idx, &paramDef)))
                {
                    WCHAR wParamName[mdNameLen] = W("\0");
                    pMD->GetParamProps(paramDef, NULL, NULL, wParamName, mdNameLen, &paramNameLen, NULL, NULL, NULL, NULL);
                    paramName = to_utf8(wParamName);
                }
            }
            if(paramName.empty())
            {
                std::ostringstream ss;
                ss << "param_" << i;
                paramName = ss.str();
            }

            ToRelease<ICorDebugValue> pValue;
            ULONG cArgsFetched;
            if (FAILED(pParamEnum->Next(1, &pValue, &cArgsFetched)))
                continue;

            if (thisParam)
            {
                // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
                pFrame.Free();
                pILFrame.Free();

                IfFailRet(HandleSpecialThisParam(pValue, locals, cb));
                if (Status == S_OK)
                    continue;
            }

            locals.insert(paramName);

            // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
            pFrame.Free();
            pILFrame.Free();

            auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
            {
                pValue->AddRef();
                *ppResultValue = pValue;
                return S_OK;
            };

            IfFailRet(cb(paramName, getValue));
        }
    }

    if (cLocals > 0)
    {
        for (ULONG i = 0; i < cLocals; i++)
        {
            std::string paramName;
            ULONG32 ilStart;
            ULONG32 ilEnd;
            if (FAILED(m_sharedModules->GetFrameNamedLocalVariable(pModule, methodDef, i, paramName, &ilStart, &ilEnd)))
                continue;

            if (currentIlOffset < ilStart || currentIlOffset >= ilEnd)
                continue;

            if (!pFrame) // Forced to update pFrame/pILFrame.
            {
                IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
                if (pFrame == nullptr)
                    return E_FAIL;
                IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
            }
            ToRelease<ICorDebugValue> pValue;
            if (FAILED(pILFrame->GetLocalVariable(i, &pValue)) || !pValue)
                continue;

            // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
            pFrame.Free();
            pILFrame.Free();

            IfFailRet(HandleSpecialLocalVar(paramName, pValue, locals, cb));
            if (Status == S_OK)
                continue;

            auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
            {
                pValue->AddRef();
                *ppResultValue = pValue;
                return S_OK;
            };

            locals.insert(paramName);
            IfFailRet(cb(paramName, getValue));
        }
    }

    return S_OK;
}

HRESULT Evaluator::SetValue(
    ICorDebugValue *pValue,
    const std::string &value,
    ICorDebugThread *pThread,
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
        IfFailRet(Interop::ParseExpression(value, renamed->second, data, errorText));
    }
    else if (corType == ELEMENT_TYPE_STRING)
    {
        IfFailRet(Interop::ParseExpression(value, "System.String", data, errorText));

        ToRelease<ICorDebugValue> pNewString;
        IfFailRet(m_sharedEvalHelpers->CreateString(pThread, data, &pNewString));

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
        IfFailRet(Interop::ParseExpression(value, "System.Decimal", data, errorText));
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
