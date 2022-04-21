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
#include "debugger/evalstackmachine.h"
#include "debugger/frames.h"
#include "utils/utf.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "valueprint.h"
#include "managed/interop.h"

namespace netcoredbg
{

HRESULT Evaluator::GetElement(ICorDebugValue *pInputValue, std::vector<ULONG32> &indexes, ICorDebugValue **ppResultValue)
{
    HRESULT Status;

    if (indexes.empty())
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

    if (indexes.size() != nRank)
        return E_FAIL;

    return pArrayVal->GetElement(static_cast<uint32_t>(indexes.size()), indexes.data(), ppResultValue);
}

HRESULT Evaluator::FollowFields(ICorDebugThread *pThread,
                                FrameLevel frameLevel,
                                ICorDebugValue *pValue,
                                ValueKind valueKind,
                                std::vector<std::string> &identifiers,
                                int nextIdentifier,
                                ICorDebugValue **ppResult,
                                std::unique_ptr<SetterData> *resultSetterData,
                                int evalFlags)
{
    HRESULT Status;

    // Note, in case of (nextIdentifier == identifiers.size()) result is pValue itself, so, we ok here.
    if (nextIdentifier > (int)identifiers.size())
        return E_FAIL;

    pValue->AddRef();
    ToRelease<ICorDebugValue> pResultValue(pValue);
    for (int i = nextIdentifier; i < (int)identifiers.size(); i++)
    {
        if (identifiers[i].empty())
            return E_FAIL;

        ToRelease<ICorDebugValue> pClassValue(std::move(pResultValue));

        WalkMembers(pClassValue, pThread, frameLevel, !!resultSetterData, [&](
            ICorDebugType *pType,
            bool is_static,
            const std::string &memberName,
            GetValueCallback getValue,
            SetterData *setterData)
        {
            if (is_static && valueKind == ValueIsVariable)
                return S_OK;
            if (!is_static && valueKind == ValueIsClass)
                return S_OK;

            if (memberName != identifiers[i])
                return S_OK;

            IfFailRet(getValue(&pResultValue, evalFlags));
            if (setterData && resultSetterData)
                (*resultSetterData).reset(new SetterData(*setterData));

            return E_ABORT; // Fast exit from cycle with result.
        });

        if (!pResultValue)
            return E_FAIL;

        valueKind = ValueIsVariable; // we can only follow through instance fields
    }

    *ppResult = pResultValue.Detach();
    return S_OK;
}

HRESULT Evaluator::FollowNestedFindValue(ICorDebugThread *pThread,
                                         FrameLevel frameLevel,
                                         const std::string &methodClass,
                                         std::vector<std::string> &identifiers,
                                         ICorDebugValue **ppResult,
                                         std::unique_ptr<SetterData> *resultSetterData,
                                         int evalFlags)
{
    HRESULT Status;

    std::vector<int> ranks;
    std::vector<std::string> classIdentifiers = EvalUtils::ParseType(methodClass, ranks);
    int nextClassIdentifier = 0;
    int identifiersNum = (int)identifiers.size() - 1;
    std::vector<std::string> fieldName {identifiers.back()};
    std::vector<std::string> fullpath;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(EvalUtils::FindType(classIdentifiers, nextClassIdentifier, pThread, m_sharedModules.get(), nullptr, nullptr, &pModule));

    bool trim = false;
    while (!classIdentifiers.empty())
    {
        ToRelease<ICorDebugType> pType;
        nextClassIdentifier = 0;
        if (trim)
            classIdentifiers.pop_back();

        fullpath = classIdentifiers;
        for (int i = 0; i < identifiersNum; i++)
            fullpath.push_back(identifiers[i]);

        if (FAILED(EvalUtils::FindType(fullpath, nextClassIdentifier, pThread, m_sharedModules.get(), pModule, &pType)))  // NOLINT(clang-analyzer-cplusplus.Move)
            break;

        if (nextClassIdentifier < (int)fullpath.size())
        {
            // try to check non-static fields inside a static member
            std::vector<std::string> staticName;
            for (int i = nextClassIdentifier; i < (int)fullpath.size(); i++)
            {
                staticName.emplace_back(fullpath[i]);
            }
            staticName.emplace_back(fieldName[0]);
            ToRelease<ICorDebugValue> pTypeObject;
            if (S_OK == m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject))
            {
                if (SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, staticName, 0, ppResult, resultSetterData, evalFlags)))
                    return S_OK;
            }
            trim = true;
            continue;
        }

        ToRelease<ICorDebugValue> pTypeObject;
        IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject));
        if (Status == S_OK && // type have static members (S_FALSE if type don't have static members)
            SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, fieldName, 0, ppResult, resultSetterData, evalFlags)))
            return S_OK;

        trim = true;
    }

    return E_FAIL;
}

static HRESULT FollowNestedFindType(ICorDebugThread *pThread,
                                    Modules *pModules,
                                    const std::string &methodClass,
                                    std::vector<std::string> &identifiers,
                                    ICorDebugType **ppResultType)
{
    HRESULT Status;

    std::vector<int> ranks;
    std::vector<std::string> classIdentifiers = EvalUtils::ParseType(methodClass, ranks);
    int nextClassIdentifier = 0;
    std::vector<std::string> fullpath;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(EvalUtils::FindType(classIdentifiers, nextClassIdentifier, pThread, pModules, nullptr, nullptr, &pModule));

    bool trim = false;
    while (!classIdentifiers.empty())
    {
        if (trim)
            classIdentifiers.pop_back();

        fullpath = classIdentifiers;
        for (auto &identifier : identifiers)
            fullpath.push_back(identifier);

        nextClassIdentifier = 0;
        ToRelease<ICorDebugType> pType;
        if (FAILED(EvalUtils::FindType(fullpath, nextClassIdentifier, pThread, pModules, pModule, &pType)))  // NOLINT(clang-analyzer-cplusplus.Move)
            break;

        if (nextClassIdentifier == (int)fullpath.size())
        {
            *ppResultType = pType.Detach();
            return S_OK;
        }

        trim = true;
    }

    return E_FAIL;
}

HRESULT Evaluator::ResolveIdentifiers(ICorDebugThread *pThread,
                                      FrameLevel frameLevel,
                                      ICorDebugValue *pInputValue,
                                      SetterData *inputSetterData,
                                      std::vector<std::string> &identifiers,
                                      ICorDebugValue **ppResultValue,
                                      std::unique_ptr<SetterData> *resultSetterData,
                                      ICorDebugType **ppResultType,
                                      int evalFlags)
{
    if (pInputValue && identifiers.empty())
    {
        pInputValue->AddRef();
        *ppResultValue = pInputValue;
        if (inputSetterData && resultSetterData)
            (*resultSetterData).reset(new SetterData(*inputSetterData));
        return S_OK;
    }
    else if (pInputValue)
    {
        return FollowFields(pThread, frameLevel, pInputValue, Evaluator::ValueIsVariable, identifiers, 0, ppResultValue, resultSetterData, evalFlags);
    }

    HRESULT Status;
    int nextIdentifier = 0;
    ToRelease<ICorDebugValue> pResolvedValue;
    ToRelease<ICorDebugValue> pThisValue;

    if (identifiers.at(nextIdentifier) == "$exception")
    {
        IfFailRet(pThread->GetCurrentException(&pResolvedValue));
        if (pResolvedValue == nullptr)
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

                if (name == identifiers.at(nextIdentifier))
                    return E_ABORT; // Fast way to exit from stack vars walk routine.
            }
            else if (name == identifiers.at(nextIdentifier))
            {
                if (FAILED(getValue(&pResolvedValue, evalFlags)) || !pResolvedValue)
                    return S_OK;

                return E_ABORT; // Fast way to exit from stack vars walk routine.
            }

            return S_OK;
        })) && !pThisValue && !pResolvedValue) // Check, that we have fast exit instead of real error.
        {
            return Status;
        }
    }

    if (!pResolvedValue && pThisValue) // check this/this.*
    {
        if (identifiers[nextIdentifier] == "this")
            nextIdentifier++; // skip first identifier with "this" (we have it in pThisValue), check rest

        if (SUCCEEDED(FollowFields(pThread, frameLevel, pThisValue, ValueIsVariable, identifiers, nextIdentifier, &pResolvedValue, resultSetterData, evalFlags)))
        {
            *ppResultValue = pResolvedValue.Detach();
            return S_OK;
        }
    }

    if (!pResolvedValue) // check statics in nested classes
    {
        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
        if (pFrame == nullptr)
            return E_FAIL;

        std::string methodClass;
        std::string methodName;
        TypePrinter::GetTypeAndMethod(pFrame, methodClass, methodName);

        if (SUCCEEDED(FollowNestedFindValue(pThread, frameLevel, methodClass, identifiers, &pResolvedValue, resultSetterData, evalFlags)))
        {
            *ppResultValue = pResolvedValue.Detach();
            return S_OK;
        }

        if (ppResultType && 
            SUCCEEDED(FollowNestedFindType(pThread, m_sharedModules.get(), methodClass, identifiers, ppResultType)))
            return S_OK;
    }

    ValueKind valueKind;
    if (pResolvedValue)
    {
        nextIdentifier++;
        if (nextIdentifier == (int)identifiers.size())
        {
            *ppResultValue = pResolvedValue.Detach();
            return S_OK;
        }
        valueKind = ValueIsVariable;
    }
    else
    {
        ToRelease<ICorDebugType> pType;
        IfFailRet(EvalUtils::FindType(identifiers, nextIdentifier, pThread, m_sharedModules.get(), nullptr, &pType));
        IfFailRet(m_sharedEvalHelpers->CreatTypeObjectStaticConstructor(pThread, pType, &pResolvedValue));

        // Identifiers resolved into type, not value. In case type could be result - provide type directly as result.
        // In this way caller will know, that no object instance here (should operate with static members/methods only).
        if (ppResultType && nextIdentifier == (int)identifiers.size())
        {
            *ppResultType = pType.Detach();
            return S_OK;
        }

        if (Status == S_FALSE || // type don't have static members, nothing explore here
            nextIdentifier == (int)identifiers.size()) // pResolvedValue is temporary object for members exploration, can't be result
            return E_INVALIDARG;

        valueKind = ValueIsClass;
    }

    ToRelease<ICorDebugValue> pValue(std::move(pResolvedValue));
    IfFailRet(FollowFields(pThread, frameLevel, pValue, valueKind, identifiers, nextIdentifier, &pResolvedValue, resultSetterData, evalFlags));

    *ppResultValue = pResolvedValue.Detach();
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

// https://github.com/dotnet/runtime/blob/57bfe474518ab5b7cfe6bf7424a79ce3af9d6657/docs/design/coreclr/profiling/davbr-blog-archive/samples/sigparse.cpp
// This blog post originally appeared on David Broman's blog on 10/13/2005

// Sig ::= MethodDefSig | MethodRefSig | StandAloneMethodSig | FieldSig | PropertySig | LocalVarSig
// MethodDefSig ::= [[HASTHIS] [EXPLICITTHIS]] (DEFAULT|VARARG|GENERIC GenParamCount) ParamCount RetType Param*
// MethodRefSig ::= [[HASTHIS] [EXPLICITTHIS]] VARARG ParamCount RetType Param* [SENTINEL Param+]
// StandAloneMethodSig ::= [[HASTHIS] [EXPLICITTHIS]] (DEFAULT|VARARG|C|STDCALL|THISCALL|FASTCALL) ParamCount RetType Param* [SENTINEL Param+]
// FieldSig ::= FIELD CustomMod* Type
// PropertySig ::= PROPERTY [HASTHIS] ParamCount CustomMod* Type Param*
// LocalVarSig ::= LOCAL_SIG Count (TYPEDBYREF | ([CustomMod] [Constraint])* [BYREF] Type)+ 

// -------------

// CustomMod ::= ( CMOD_OPT | CMOD_REQD ) ( TypeDefEncoded | TypeRefEncoded )
// Constraint ::= #define ELEMENT_TYPE_PINNED
// Param ::= CustomMod* ( TYPEDBYREF | [BYREF] Type )
// RetType ::= CustomMod* ( VOID | TYPEDBYREF | [BYREF] Type )
// Type ::= ( BOOLEAN | CHAR | I1 | U1 | U2 | U2 | I4 | U4 | I8 | U8 | R4 | R8 | I | U |
// | VALUETYPE TypeDefOrRefEncoded
// | CLASS TypeDefOrRefEncoded
// | STRING 
// | OBJECT
// | PTR CustomMod* VOID
// | PTR CustomMod* Type
// | FNPTR MethodDefSig
// | FNPTR MethodRefSig
// | ARRAY Type ArrayShape
// | SZARRAY CustomMod* Type
// | GENERICINST (CLASS | VALUETYPE) TypeDefOrRefEncoded GenArgCount Type*
// | VAR Number
// | MVAR Number

// ArrayShape ::= Rank NumSizes Size* NumLoBounds LoBound*

// TypeDefOrRefEncoded ::= TypeDefEncoded | TypeRefEncoded
// TypeDefEncoded ::= 32-bit-3-part-encoding-for-typedefs-and-typerefs
// TypeRefEncoded ::= 32-bit-3-part-encoding-for-typedefs-and-typerefs

// ParamCount ::= 29-bit-encoded-integer
// GenArgCount ::= 29-bit-encoded-integer
// Count ::= 29-bit-encoded-integer
// Rank ::= 29-bit-encoded-integer
// NumSizes ::= 29-bit-encoded-integer
// Size ::= 29-bit-encoded-integer
// NumLoBounds ::= 29-bit-encoded-integer
// LoBounds ::= 29-bit-encoded-integer
// Number ::= 29-bit-encoded-integer

static HRESULT ParseElementType(IMetaDataImport *pMD, PCCOR_SIGNATURE *ppSig, Evaluator::ArgElementType &argElementType)
{
    HRESULT Status;
    ULONG corType;
    mdToken tk;
    *ppSig += CorSigUncompressData(*ppSig, &corType);
    argElementType.corType = (CorElementType)corType;

    switch (argElementType.corType)
    {
        case ELEMENT_TYPE_VOID:
        case ELEMENT_TYPE_BOOLEAN:
        case ELEMENT_TYPE_CHAR:
        case ELEMENT_TYPE_I1:
        case ELEMENT_TYPE_U1:
        case ELEMENT_TYPE_I2:
        case ELEMENT_TYPE_U2:
        case ELEMENT_TYPE_I4:
        case ELEMENT_TYPE_U4:
        case ELEMENT_TYPE_I8:
        case ELEMENT_TYPE_U8:
        case ELEMENT_TYPE_R4:
        case ELEMENT_TYPE_R8:
        case ELEMENT_TYPE_STRING:
        case ELEMENT_TYPE_OBJECT:
            break;

        case ELEMENT_TYPE_VALUETYPE:
        case ELEMENT_TYPE_CLASS:
            *ppSig += CorSigUncompressToken(*ppSig, &tk);
            IfFailRet(TypePrinter::NameForTypeByToken(tk, pMD, argElementType.typeName, nullptr));
            break;

// TODO
        case ELEMENT_TYPE_U: // "nuint" - error CS8652: The feature 'native-sized integers' is currently in Preview and *unsupported*. To use Preview features, use the 'preview' language version.
        case ELEMENT_TYPE_I: // "nint" - error CS8652: The feature 'native-sized integers' is currently in Preview and *unsupported*. To use Preview features, use the 'preview' language version.
        case ELEMENT_TYPE_TYPEDBYREF:
        case ELEMENT_TYPE_ARRAY:
        case ELEMENT_TYPE_PTR: // int* ptr (unsafe code only)
        case ELEMENT_TYPE_BYREF: // ref, in, out
        case ELEMENT_TYPE_SZARRAY:
        case ELEMENT_TYPE_VAR: // Generic parameter in a generic type definition, represented as number
        case ELEMENT_TYPE_MVAR: // Generic parameter in a generic method definition, represented as number
        case ELEMENT_TYPE_GENERICINST: // A type modifier for generic types - List<>, Dictionary<>, ...
        case ELEMENT_TYPE_CMOD_REQD:
        case ELEMENT_TYPE_CMOD_OPT:
            return S_FALSE;

        default:
            return E_INVALIDARG;
    }

    return S_OK;
}

HRESULT Evaluator::WalkMethods(ICorDebugValue *pInputTypeValue, WalkMethodsCallback cb)
{
    HRESULT Status;
    ToRelease<ICorDebugValue2> iCorValue2;
    IfFailRet(pInputTypeValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
    ToRelease<ICorDebugType> iCorType;
    IfFailRet(iCorValue2->GetExactType(&iCorType));

    return WalkMethods(iCorType, cb);
}

HRESULT Evaluator::WalkMethods(ICorDebugType *pInputType, WalkMethodsCallback cb)
{
    HRESULT Status;
    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pInputType->GetClass(&pClass));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    mdTypeDef currentTypeDef;
    IfFailRet(pClass->GetToken(&currentTypeDef));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numMethods = 0;
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef;
    while(SUCCEEDED(pMD->EnumMethods(&fEnum, currentTypeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {

        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};
        DWORD methodAttr = 0;
        PCCOR_SIGNATURE pSig = NULL;
        ULONG cbSig = 0;
        if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef,
                                     szFunctionName, _countof(szFunctionName), &nameLen,
                                     &methodAttr, &pSig, &cbSig, nullptr,  nullptr)))
            continue;

        ULONG cParams; // Count of signature parameters.
        ULONG elementSize;
        ULONG convFlags;

        // 1. calling convention for MethodDefSig:
        // [[HASTHIS] [EXPLICITTHIS]] (DEFAULT|VARARG|GENERIC GenParamCount)
        elementSize = CorSigUncompressData(pSig, &convFlags);
        pSig += elementSize;

        // 2. count of params
        elementSize = CorSigUncompressData(pSig, &cParams);
        pSig += elementSize;

        // 3. return type
        ArgElementType returnElementType;
        IfFailRet(ParseElementType(pMD, &pSig, returnElementType));
        if (Status == S_FALSE)
            continue;

        // 4. get next element from method signature
        std::vector<ArgElementType> argElementTypes(cParams);
        for (ULONG i = 0; i < cParams; ++i)
        {
            IfFailRet(ParseElementType(pMD, &pSig, argElementTypes[i]));
            if (Status == S_FALSE)
                break;
        }
        if (Status == S_FALSE)
            continue;

        bool is_static = (methodAttr & mdStatic);

        auto getFunction = [&](ICorDebugFunction **ppResultFunction) -> HRESULT
        {
            return pModule->GetFunctionFromToken(methodDef, ppResultFunction);
        };

        Status = cb(is_static, to_utf8(szFunctionName), returnElementType, argElementTypes, getFunction);
        if (FAILED(Status))
        {
            pMD->CloseEnum(fEnum);
            return Status;
        }
    }
    pMD->CloseEnum(fEnum);

    ToRelease<ICorDebugType> iCorBaseType;
    if(SUCCEEDED(pInputType->GetBase(&iCorBaseType)) && iCorBaseType != NULL)
    {
        IfFailRet(WalkMethods(iCorBaseType, cb));
    }

    return S_OK;
}

HRESULT Evaluator::SetValue(ICorDebugThread *pThread, FrameLevel frameLevel, ICorDebugValue *pValue, SetterData *setterData,
                            const std::string &value, int evalFlags, std::string &output)
{
    if (!pThread)
        return E_FAIL;

    // In case this is not property, just change value itself.
    if (!setterData)
        return m_sharedEvalStackMachine->SetValueByExpression(pThread, frameLevel, evalFlags, pValue, value, output);

    HRESULT Status;
    pValue->AddRef();
    ToRelease<ICorDebugValue> iCorValue(pValue);
    CorElementType corType;
    IfFailRet(iCorValue->GetType(&corType));

    if (corType == ELEMENT_TYPE_STRING)
    {
        // FIXME investigate, why in this case we can't use ICorDebugReferenceValue::SetValue() for string in iCorValue
        iCorValue.Free();
        IfFailRet(m_sharedEvalStackMachine->EvaluateExpression(pThread, frameLevel, evalFlags, value, &iCorValue, output));

        CorElementType elemType;
        IfFailRet(iCorValue->GetType(&elemType));
        if (elemType != ELEMENT_TYPE_STRING)
            return E_INVALIDARG;
    }
    else // Allow stack machine decide what types are supported.
    {
        IfFailRet(m_sharedEvalStackMachine->SetValueByExpression(pThread, frameLevel, evalFlags, iCorValue.GetPtr(), value, output));
    }

    // Call setter.
    if (!setterData->thisValue)
    {
        return m_sharedEvalHelpers->EvalFunction(pThread, setterData->setterFunction, nullptr, 0, iCorValue.GetRef(), 1, nullptr, evalFlags);
    }
    else
    {
        ICorDebugValue *ppArgsValue[] = {setterData->thisValue, iCorValue};
        return m_sharedEvalHelpers->EvalFunction(pThread, setterData->setterFunction, nullptr, 0, ppArgsValue, 2, nullptr, evalFlags);
    }
}

// https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameParser.cs#L13
static bool IsSynthesizedLocalName(WCHAR *mdName, ULONG nameLen)
{
    return (nameLen > 1 && starts_with(mdName, W("<"))) ||
           (nameLen > 4 && starts_with(mdName, W("CS$<")));
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pInputValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    ICorDebugType *pTypeCast,
    bool provideSetterData,
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

        return cb(nullptr, false, "", getValue, nullptr);
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

            IfFailRet(cb(nullptr, false, "[" + IndiciesToStr(ind, base) + "]", getValue, nullptr));
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
            if (IsSynthesizedLocalName(mdName, nameLen))
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

            IfFailRet(cb(pType, is_static, name, getValue, nullptr));
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

            if (provideSetterData)
            {
                ToRelease<ICorDebugFunction> iCorFuncSetter;
                if (FAILED(pModule->GetFunctionFromToken(mdSetter, &iCorFuncSetter)))
                {
                    iCorFuncSetter.Free();
                }
                SetterData setterData(is_static ? nullptr : pInputValue, iCorFuncSetter);
                IfFailRet(cb(pType, is_static, name, getValue, &setterData));
            }
            else
            {
                IfFailRet(cb(pType, is_static, name, getValue, nullptr));
            }
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
            IfFailRet(WalkMembers(pInputValue, pThread, frameLevel, pBaseType, provideSetterData, cb));
        }
    }

    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    bool provideSetterData,
    WalkMembersCallback cb)
{
    return WalkMembers(pValue, pThread, frameLevel, nullptr, provideSetterData, cb);
}

enum class GeneratedCodeKind
{
    Normal,
    Async,
    Lambda
};

static HRESULT GetGeneratedCodeKind(IMetaDataImport *pMD, const WSTRING methodName, mdTypeDef typeDef, GeneratedCodeKind &result)
{
    HRESULT Status;
    WCHAR name[mdNameLen];
    ULONG nameLen;
    IfFailRet(pMD->GetTypeDefProps(typeDef, name, _countof(name), &nameLen, NULL, NULL));
    WSTRING typeName(name);

    // https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameParser.cs#L20-L24
    //  Parse the generated name. Returns true for names of the form
    //  [CS$]<[middle]>c[__[suffix]] where [CS$] is included for certain
    //  generated names, where [middle] and [__[suffix]] are optional,
    //  and where c is a single character in [1-9a-z]
    //  (csharp\LanguageAnalysis\LIB\SpecialName.cpp).

    // https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameKind.cs#L13-L20
    //  LambdaMethod = 'b',
    //  LambdaDisplayClass = 'c',
    //  StateMachineType = 'd',

    // https://github.com/dotnet/roslyn/blob/21055e1858548dbd8f4c1fd5d25a9c9617873806/src/Compilers/Core/Portable/PublicAPI.Shipped.txt#L252
    //  const Microsoft.CodeAnalysis.WellKnownMemberNames.MoveNextMethodName = "MoveNext" -> string!
    //  ... used in SynthesizedStateMachineMoveNextMethod class constructor.

    if (methodName.rfind(W("MoveNext"), 0) != WSTRING::npos && typeName.find(W(">d")) != WSTRING::npos)
        result = GeneratedCodeKind::Async;
    else if (methodName.find(W(">b")) != WSTRING::npos && typeName.find(W(">c")) != WSTRING::npos)
        result = GeneratedCodeKind::Lambda;
    else
        result = GeneratedCodeKind::Normal;

    return S_OK;
}

enum class GeneratedNameKind
{
    None,
    ThisProxyField,
    HoistedLocalField,
    DisplayClassLocalOrField
};

static GeneratedNameKind GetLocalOrFieldNameKind(const WSTRING &localOrFieldName)
{
    // https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameParser.cs#L20-L24
    //  Parse the generated name. Returns true for names of the form
    //  [CS$]<[middle]>c[__[suffix]] where [CS$] is included for certain
    //  generated names, where [middle] and [__[suffix]] are optional,
    //  and where c is a single character in [1-9a-z]
    //  (csharp\LanguageAnalysis\LIB\SpecialName.cpp).

    // https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameKind.cs#L13-L20
    //  ThisProxyField = '4',
    //  HoistedLocalField = '5',
    //  DisplayClassLocalOrField = '8',

    if (localOrFieldName.find(W(">4")) != WSTRING::npos)
        return GeneratedNameKind::ThisProxyField;
    else if (localOrFieldName.find(W(">5")) != WSTRING::npos)
        return GeneratedNameKind::HoistedLocalField;
    else if (localOrFieldName.find(W(">8")) != WSTRING::npos)
        return GeneratedNameKind::DisplayClassLocalOrField;

    return GeneratedNameKind::None;
}

static HRESULT GetClassAndTypeDefByValue(ICorDebugValue *pValue, ICorDebugClass **ppClass, mdTypeDef &typeDef)
{
    HRESULT Status;
    ToRelease<ICorDebugValue2> iCorValue2;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
    ToRelease<ICorDebugType> iCorType;
    IfFailRet(iCorValue2->GetExactType(&iCorType));
    IfFailRet(iCorType->GetClass(ppClass));
    IfFailRet((*ppClass)->GetToken(&typeDef));
    return S_OK;
}

static HRESULT FindThisProxyFieldValue(IMetaDataImport *pMD, ICorDebugClass *pClass, mdTypeDef typeDef, ICorDebugValue *pInputValue, ICorDebugValue **ppResultValue)
{
    HRESULT Status;
    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));
    if (isNull == TRUE)
        return E_INVALIDARG;

    Status = ForEachFields(pMD, typeDef, [&](mdFieldDef fieldDef) -> HRESULT
    {
        WCHAR mdName[mdNameLen] = {0};
        ULONG nameLen = 0;
        if (SUCCEEDED(pMD->GetFieldProps(fieldDef, nullptr, mdName, _countof(mdName), &nameLen, NULL, NULL, NULL, NULL, NULL, NULL)))
        {
            auto getValue = [&](ICorDebugValue **ppResultValue) -> HRESULT
            {
                ToRelease<ICorDebugObjectValue> pObjValue;
                IfFailRet(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue));
                IfFailRet(pObjValue->GetFieldValue(pClass, fieldDef, ppResultValue));
                return S_OK;
            };

            GeneratedNameKind generatedNameKind = GetLocalOrFieldNameKind(mdName);
            if (generatedNameKind == GeneratedNameKind::ThisProxyField)
            {
                IfFailRet(getValue(ppResultValue));
                return E_ABORT; // Fast exit from cycle
            }
            else if (generatedNameKind == GeneratedNameKind::DisplayClassLocalOrField)
            {
                ToRelease<ICorDebugValue> iCorDisplayClassValue;
                IfFailRet(getValue(&iCorDisplayClassValue));
                ToRelease<ICorDebugClass> iCorDisplayClass;
                mdTypeDef displayClassTypeDef;
                IfFailRet(GetClassAndTypeDefByValue(iCorDisplayClassValue, &iCorDisplayClass, displayClassTypeDef));
                IfFailRet(FindThisProxyFieldValue(pMD, iCorDisplayClass, displayClassTypeDef, iCorDisplayClassValue, ppResultValue));
                if (ppResultValue)
                    return E_ABORT; // Fast exit from cycle
            }
        }
        return S_OK; // Return with success to continue walk.
    });

    return Status == E_ABORT ? S_OK : Status;
}

// Note, this method return Class name, not Type name (will not provide generic initialization types if any).
HRESULT Evaluator::GetMethodClass(ICorDebugThread *pThread, FrameLevel frameLevel, std::string &methodClass, bool &haveThis)
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

    DWORD methodAttr = 0;
    WCHAR szMethod[mdNameLen];
    ULONG szMethodLen;
    IfFailRet(pMD->GetMethodProps(methodDef, NULL, szMethod, _countof(szMethod), &szMethodLen, &methodAttr, NULL, NULL, NULL, NULL));

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pFunction->GetClass(&pClass));
    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));
    // We are inside method of this class, if typeDef is not TypeDef token - something definitely going wrong.
    if (TypeFromToken(typeDef) != mdtTypeDef)
        return E_FAIL;

    haveThis = (methodAttr & mdStatic) == 0;
    // In case this is static method, this is not async/lambda case for sure.
    if (!haveThis)
        return TypePrinter::NameForTypeDef(typeDef, pMD, methodClass, nullptr);

    GeneratedCodeKind generatedCodeKind;
    IfFailRet(GetGeneratedCodeKind(pMD, szMethod, typeDef, generatedCodeKind));
    if (generatedCodeKind == GeneratedCodeKind::Normal)
        return TypePrinter::NameForTypeDef(typeDef, pMD, methodClass, nullptr);

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
    ToRelease<ICorDebugValue> currentThis;
    IfFailRet(pILFrame->GetArgument(0, &currentThis));

    // Check do we have real This value (that should be stored in ThisProxyField).
    ToRelease<ICorDebugValue> userThis;
    IfFailRet(FindThisProxyFieldValue(pMD, pClass, typeDef, currentThis, &userThis));
    haveThis = !!userThis;

    // Find first user code enclosing class, since compiler add async/lambda as nested class.
    do {
        ULONG nameLen;
        WCHAR mdName[mdNameLen];
        IfFailRet(pMD->GetTypeDefProps(typeDef, mdName, _countof(mdName), &nameLen, NULL, NULL));

        if (!IsSynthesizedLocalName(mdName, nameLen))
            break;

        mdTypeDef enclosingClass;
        if (SUCCEEDED(Status = pMD->GetNestedClassProps(typeDef, &enclosingClass)))
            typeDef = enclosingClass;
        else
            return Status;
    } while(1);

    return TypePrinter::NameForTypeDef(typeDef, pMD, methodClass, nullptr);
}

// https://github.com/dotnet/roslyn/blob/3fdd28bc26238f717ec1124efc7e1f9c2158bce2/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameParser.cs#L139-L159
static HRESULT TryParseSlotIndex(const WSTRING &mdName, int32_t &index)
{
    // https://github.com/dotnet/roslyn/blob/d1e617ded188343ba43d24590802dd51e68e8e32/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameConstants.cs#L11
    const WSTRING suffixSeparator(W("__"));
    WSTRING::size_type suffixSeparatorOffset = mdName.rfind(suffixSeparator);
    if (suffixSeparatorOffset == WSTRING::npos)
        return E_FAIL;

    WSTRING slotIndexString = mdName.substr(suffixSeparatorOffset + 2 /* suffixSeparator size */);
    if (slotIndexString.empty() ||
        // Slot index is positive 4 byte int, that mean max is 10 characters (2147483647).
        slotIndexString.size() > 10)
        return E_FAIL;

    int32_t slotIndex = 0;
    for (WSTRING::size_type i = 0; i < slotIndexString.size(); i++)
    {
        if (slotIndexString[i] < W('0') && slotIndexString[i] > W('9'))
            return E_FAIL;

        slotIndex = slotIndex * 10 + (int32_t)(slotIndexString[i] -  W('0'));
    }

    if (slotIndex < 1) // Slot index start from 1.
        return E_FAIL;

    index = slotIndex - 1;
    return S_OK;
}

// https://github.com/dotnet/roslyn/blob/3fdd28bc26238f717ec1124efc7e1f9c2158bce2/src/Compilers/CSharp/Portable/Symbols/Synthesized/GeneratedNameParser.cs#L20-L59
static HRESULT TryParseHoistedLocalName(const WSTRING &mdName, WSTRING &wLocalName)
{
    WSTRING::size_type nameStartOffset;
    if (mdName.length() > 1 && starts_with(mdName.data(), W("<")))
        nameStartOffset = 1;
    else if (mdName.length() > 4 && starts_with(mdName.data(), W("CS$<")))
        nameStartOffset = 4;
    else
        return E_FAIL;

    WSTRING::size_type closeBracketOffset = mdName.find('>', nameStartOffset);
    if (closeBracketOffset == WSTRING::npos)
        return E_FAIL;

    wLocalName = mdName.substr(nameStartOffset, closeBracketOffset - nameStartOffset);
    return S_OK;
}

static HRESULT WalkGeneratedClassFields(IMetaDataImport *pMD, ICorDebugValue *pInputValue, ULONG32 currentIlOffset, std::unordered_set<WSTRING> &usedNames, 
                                        mdMethodDef methodDef, ULONG32 methodVersion, Modules *pModules, ICorDebugModule *pModule, Evaluator::WalkStackVarsCallback cb)
{
    HRESULT Status;
    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));
    if (isNull == TRUE)
        return S_OK;

    ToRelease<ICorDebugClass> pClass;
    mdTypeDef currentTypeDef;
    IfFailRet(GetClassAndTypeDefByValue(pValue, &pClass, currentTypeDef));

    struct hoisted_local_scope_t
    {
        uint32_t startOffset;
        uint32_t length;
    };
    struct hoisted_local_scope_t_deleter
    {
        void operator()(hoisted_local_scope_t *p) const
        {
            Interop::CoTaskMemFree(p);
        }
    };

    int32_t hoistedLocalScopesCount = -1;
    std::unique_ptr<hoisted_local_scope_t, hoisted_local_scope_t_deleter> hoistedLocalScopes;

    IfFailRet(ForEachFields(pMD, currentTypeDef, [&](mdFieldDef fieldDef) -> HRESULT
    {
        WCHAR mdName[mdNameLen] = {0};
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        if (FAILED(pMD->GetFieldProps(fieldDef, nullptr, mdName, _countof(mdName), &nameLen, &fieldAttr, NULL, NULL, NULL, NULL, NULL)))
            return S_OK; // Return with success to continue walk.

        if ((fieldAttr & fdStatic) != 0 || (fieldAttr & fdLiteral) != 0 ||
            usedNames.find(mdName) != usedNames.end())
            return S_OK; // Return with success to continue walk.

        auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
        {
            // Get pValue again, since it could be neutered at eval call in `cb` on previous cycle.
            pValue.Free();
            IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));
            ToRelease<ICorDebugObjectValue> pObjValue;
            IfFailRet(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue));
            IfFailRet(pObjValue->GetFieldValue(pClass, fieldDef, ppResultValue));
            return S_OK;
        };

        GeneratedNameKind generatedNameKind = GetLocalOrFieldNameKind(mdName);
        if (generatedNameKind == GeneratedNameKind::DisplayClassLocalOrField)
        {
            ToRelease<ICorDebugValue> iCorDisplayClassValue;
            IfFailRet(getValue(&iCorDisplayClassValue, defaultEvalFlags));
            IfFailRet(WalkGeneratedClassFields(pMD, iCorDisplayClassValue, currentIlOffset, usedNames, methodDef, methodVersion, pModules, pModule, cb));
        }
        else if (generatedNameKind == GeneratedNameKind::HoistedLocalField)
        {
            if (hoistedLocalScopesCount == -1)
            {
                PVOID data = nullptr;
                if (SUCCEEDED(pModules->GetHoistedLocalScopes(pModule, methodDef, methodVersion, &data, hoistedLocalScopesCount)) && data)
                    hoistedLocalScopes.reset((hoisted_local_scope_t*)data);
                else
                    hoistedLocalScopesCount = 0;
            }

            // Check, that hoisted local is in scope.
            // Note, in case we have any issue - ignore this check and show variable, since this is not fatal error.
            int32_t index;
            if (hoistedLocalScopesCount > 0 &&
                SUCCEEDED(TryParseSlotIndex(mdName, index)) &&
                hoistedLocalScopesCount > index &&
                (currentIlOffset < hoistedLocalScopes.get()[index].startOffset ||
                 currentIlOffset >= hoistedLocalScopes.get()[index].startOffset + hoistedLocalScopes.get()[index].length))
                return S_OK; // Return with success to continue walk.

            WSTRING wLocalName;
            if (FAILED(TryParseHoistedLocalName(mdName, wLocalName)))
                return S_OK; // Return with success to continue walk.

            IfFailRet(cb(to_utf8(wLocalName.data()), getValue));
            usedNames.insert(wLocalName);
        }
        // Ignore any other compiler generated fields, show only normal fields.
        else if (!IsSynthesizedLocalName(mdName, nameLen))
        {
            IfFailRet(cb(to_utf8(mdName), getValue));
            usedNames.insert(mdName);
        }
        return S_OK; // Return with success to continue walk.
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

    ULONG32 currentIlOffset;
    Modules::SequencePoint sp;
    // GetFrameILAndSequencePoint() return "success" code only in case it found sequence point
    // for current IP, that mean we stop inside user code.
    // Note, we could have request for not user code, we ignore it and this is OK.
    if (FAILED(m_sharedModules->GetFrameILAndSequencePoint(pFrame, currentIlOffset, sp)))
        return S_OK;

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunction->GetILCode(&pCode));
    ULONG32 methodVersion;
    IfFailRet(pCode->GetVersionNumber(&methodVersion));

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

    ToRelease<ICorDebugValueEnum> pLocalsEnum;
    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));

    ULONG cLocals = 0;
    IfFailRet(pLocalsEnum->GetCount(&cLocals));

    ULONG cArguments = 0;
    ToRelease<ICorDebugValueEnum> iCorArgumentEnum;
    IfFailRet(pILFrame->EnumerateArguments(&iCorArgumentEnum));
    IfFailRet(iCorArgumentEnum->GetCount(&cArguments));

    // Note, we use same order as vsdbg use:
    // 1. "this" (real or "this" proxy field in case async method and lambda).
    // 2. "real" argumens.
    // 3. "real" local variables.
    // 4. async/lambda object fields.

    DWORD methodAttr = 0;
    WCHAR szMethod[mdNameLen];
    ULONG szMethodLen;
    IfFailRet(pMD->GetMethodProps(methodDef, NULL, szMethod, _countof(szMethod), &szMethodLen, &methodAttr, NULL, NULL, NULL, NULL));

    GeneratedCodeKind generatedCodeKind = GeneratedCodeKind::Normal;
    ToRelease<ICorDebugValue> currentThis; // Current This. Note, in case async method or lambda - this is special object (not user's "this").
    // In case this is static method, this is not async/lambda case for sure.
    if ((methodAttr & mdStatic) == 0)
    {
        ToRelease<ICorDebugClass> pClass;
        IfFailRet(pFunction->GetClass(&pClass));
        mdTypeDef typeDef;
        IfFailRet(pClass->GetToken(&typeDef));
        IfFailRet(GetGeneratedCodeKind(pMD, szMethod, typeDef, generatedCodeKind));
        IfFailRet(pILFrame->GetArgument(0, &currentThis));

        ToRelease<ICorDebugValue> userThis;
        if (generatedCodeKind == GeneratedCodeKind::Normal)
        {
            currentThis->AddRef();
            userThis = currentThis.GetPtr();
        }
        else
        {
            // Check do we have real This value (that should be stored in ThisProxyField).
            IfFailRet(FindThisProxyFieldValue(pMD, pClass, typeDef, currentThis, &userThis));
        }

        if (userThis)
        {
            auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
            {
                userThis->AddRef();
                *ppResultValue = userThis;
                return S_OK;
            };
            IfFailRet(cb("this", getValue));
            // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
            pFrame.Free();
            pILFrame.Free();
        }
    }

    // Lambda could duplicate arguments into display class local object. Make sure we call "cb" only once for unique name.
    // Note, we don't use usedNames with 'this' related code above, since it have logic "find first and return".
    // In the same time, all code below ignore 'this' argument/field check.
    std::unordered_set<WSTRING> usedNames;

    for (ULONG i = (methodAttr & mdStatic) == 0 ? 1 : 0; i < cArguments; i++)
    {
        // https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/metadata/imetadataimport-getparamformethodindex-method
        // The ordinal position in the parameter list where the requested parameter occurs. Parameters are numbered starting from one, with the method's return value in position zero.
        // Note, IMetaDataImport::GetParamForMethodIndex() don't include "this", but ICorDebugILFrame::GetArgument() do. This is why we have different logic here.
        int idx = ((methodAttr & mdStatic) == 0) ? i : (i + 1);
        WCHAR wParamName[mdNameLen] = W("\0");
        ULONG paramNameLen = 0;
        mdParamDef paramDef;
        if(FAILED(pMD->GetParamForMethodIndex(methodDef, idx, &paramDef)) ||
           FAILED(pMD->GetParamProps(paramDef, NULL, NULL, wParamName, mdNameLen, &paramNameLen, NULL, NULL, NULL, NULL)))
           continue;

        auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
        {
            if (!pFrame) // Forced to update pFrame/pILFrame.
            {
                IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
                if (pFrame == nullptr)
                    return E_FAIL;
                IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
            }
            return pILFrame->GetArgument(i, ppResultValue);
        };

        IfFailRet(cb(to_utf8(wParamName), getValue));
        usedNames.insert(wParamName);
        // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
        pFrame.Free();
        pILFrame.Free();
    }

    for (ULONG i = 0; i < cLocals; i++)
    {
        WSTRING wLocalName;
        ULONG32 ilStart;
        ULONG32 ilEnd;
        if (FAILED(m_sharedModules->GetFrameNamedLocalVariable(pModule, methodDef, methodVersion, i, wLocalName, &ilStart, &ilEnd)))
            continue;

        if (currentIlOffset < ilStart || currentIlOffset >= ilEnd)
            continue;

        auto getValue = [&](ICorDebugValue **ppResultValue, int) -> HRESULT
        {
            if (!pFrame) // Forced to update pFrame/pILFrame.
            {
                IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
                if (pFrame == nullptr)
                    return E_FAIL;
                IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
            }
            return pILFrame->GetLocalVariable(i, ppResultValue);
        };

        // Note, this method could have lambdas inside, display class local objects must be also checked,
        // since this objects could hold current method local variables too.
        if (GetLocalOrFieldNameKind(wLocalName) == GeneratedNameKind::DisplayClassLocalOrField)
        {
            ToRelease<ICorDebugValue> iCorDisplayClassValue;
            IfFailRet(getValue(&iCorDisplayClassValue, defaultEvalFlags));
            IfFailRet(WalkGeneratedClassFields(pMD, iCorDisplayClassValue, currentIlOffset, usedNames, methodDef, methodVersion, m_sharedModules.get(), pModule, cb));
            continue;
        }

        IfFailRet(cb(to_utf8(wLocalName.data()), getValue));
        usedNames.insert(wLocalName);
        // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
        pFrame.Free();
        pILFrame.Free();
    }

    if (generatedCodeKind != GeneratedCodeKind::Normal)
        return WalkGeneratedClassFields(pMD, currentThis, currentIlOffset, usedNames, methodDef, methodVersion, m_sharedModules.get(), pModule, cb);

    return S_OK;
}

} // namespace netcoredbg
