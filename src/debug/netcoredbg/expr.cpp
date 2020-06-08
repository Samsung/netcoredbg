// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "manageddebugger.h"

#include <vector>
#include <list>

#include "cputil.h"
#include "typeprinter.h"
#include "valueprint.h"


static HRESULT ParseIndices(const std::string &s, std::vector<ULONG32> &indices)
{
    indices.clear();
    ULONG32 currentVal = 0;

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
                    indices.push_back(currentVal);
                    currentVal = 0;
                    continue;
                default:
                    return E_FAIL;
            }
        }
        currentVal *= 10;
        currentVal += (ULONG32)digit;
    }
    return S_OK;
}

HRESULT Evaluator::GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                              ICorDebugILFrame *pILFrame,
                                              ICorDebugValue *pInputValue,
                                              ValueKind valueKind,
                                              const std::string &name,
                                              ICorDebugValue **ppResultValue,
                                              int evalFlags)
{
    HRESULT Status;

    ToRelease<ICorDebugValue> pResult;

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

    IfFailRet(WalkMembers(pInputValue, pThread, pILFrame, [&](
        mdMethodDef mdGetter,
        ICorDebugModule *pModule,
        ICorDebugType *pType,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &memberName)
    {
        if (is_static && valueKind == ValueIsVariable)
            return S_OK;
        if (!is_static && valueKind == ValueIsClass)
            return S_OK;

        if (pResult)
            return S_OK;
        if (memberName != name)
            return S_OK;

        if (mdGetter != mdMethodDefNil)
        {
            ToRelease<ICorDebugFunction> pFunc;
            if (SUCCEEDED(pModule->GetFunctionFromToken(mdGetter, &pFunc)))
                EvalFunction(pThread, pFunc, pType, is_static ? nullptr : pInputValue, &pResult, evalFlags);
        }
        else
        {
            if (pValue)
                pValue->AddRef();
            pResult = pValue;
        }

        return S_OK;
    }));

    if (!pResult)
        return E_FAIL;

    *ppResultValue = pResult.Detach();
    return S_OK;
}


static mdTypeDef GetTypeTokenForName(IMetaDataImport *pMD, mdTypeDef tkEnclosingClass, const std::string &name)
{
    mdTypeDef typeToken = mdTypeDefNil;
    pMD->FindTypeDefByName(reinterpret_cast<LPCWSTR>(to_utf16(name).c_str()), tkEnclosingClass, &typeToken);
    return typeToken;
}

static HRESULT GetMethodToken(IMetaDataImport *pMD, mdTypeDef cl, const WCHAR *methodName)
{
    ULONG numMethods = 0;
    HCORENUM mEnum = NULL;
    mdMethodDef methodDef = mdTypeDefNil;
    pMD->EnumMethodsWithName(&mEnum, cl, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(mEnum);
    return methodDef;
}

HRESULT Evaluator::FindFunction(ICorDebugModule *pModule,
                            const WCHAR *typeName,
                            const WCHAR *methodName,
                            ICorDebugFunction **ppFunction)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;

    IfFailRet(pMD->FindTypeDefByName(typeName, mdTypeDefNil, &typeDef));

    mdMethodDef methodDef = GetMethodToken(pMD, typeDef, methodName);

    if (methodDef == mdMethodDefNil)
        return E_FAIL;

    return pModule->GetFunctionFromToken(methodDef, ppFunction);
}

HRESULT Evaluator::RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue, int evalFlags)
{
    HRESULT Status;

    if (!m_pRunClassConstructor && !m_pGetTypeHandle)
    {
        ToRelease<ICorDebugModule> pModule;
        IfFailRet(m_modules.GetModuleWithName("System.Private.CoreLib.dll", &pModule));

        static const WCHAR helpersName[] = W("System.Runtime.CompilerServices.RuntimeHelpers");
        static const WCHAR runCCTorMethodName[] = W("RunClassConstructor");
        static const WCHAR typeName[] = W("System.Type");
        static const WCHAR getTypeHandleMethodName[] = W("GetTypeHandle");
        IfFailRet(FindFunction(pModule, helpersName, runCCTorMethodName, &m_pRunClassConstructor));
        IfFailRet(FindFunction(pModule, typeName, getTypeHandleMethodName, &m_pGetTypeHandle));
    }

    if (!m_pRunClassConstructor || !m_pGetTypeHandle)
        return E_FAIL;

    ToRelease<ICorDebugValue> pNewValue;

    ToRelease<ICorDebugValue> pUnboxedValue;
    BOOL isNull = FALSE;
    IfFailRet(DereferenceAndUnboxValue(pValue, &pUnboxedValue, &isNull));

    CorElementType et;
    IfFailRet(pUnboxedValue->GetType(&et));

    if (et != ELEMENT_TYPE_CLASS)
        return S_OK;

    if (isNull)
    {
        ToRelease<ICorDebugValue2> pValue2;
        ToRelease<ICorDebugType> pType;

        IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
        IfFailRet(pValue2->GetExactType(&pType));

        EvalObjectNoConstructor(pThread, pType, &pNewValue, evalFlags);
    }

    ToRelease<ICorDebugValue> pRuntimeHandleValue;
    IfFailRet(EvalFunction(pThread, m_pGetTypeHandle, nullptr, pNewValue ? pNewValue.GetPtr() : pValue, &pRuntimeHandleValue, evalFlags));

    ToRelease<ICorDebugValue> pResultValue;
    IfFailRet(EvalFunction(pThread, m_pRunClassConstructor, nullptr, pRuntimeHandleValue, &pResultValue, evalFlags));

    return S_OK;
}

void Evaluator::Cleanup()
{
    if (m_pRunClassConstructor)
        m_pRunClassConstructor.Release();
    if (m_pGetTypeHandle)
        m_pGetTypeHandle.Release();
}

HRESULT Evaluator::FollowFields(ICorDebugThread *pThread,
                                ICorDebugILFrame *pILFrame,
                                ICorDebugValue *pValue,
                                ValueKind valueKind,
                                const std::vector<std::string> &parts,
                                int nextPart,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    if (nextPart >= (int)parts.size())
        return E_FAIL;

    pValue->AddRef();
    ToRelease<ICorDebugValue> pResultValue(pValue);
    for (int i = nextPart; i < (int)parts.size(); i++)
    {
        ToRelease<ICorDebugValue> pClassValue(std::move(pResultValue));
        RunClassConstructor(pThread, pClassValue, evalFlags);
        IfFailRet(GetFieldOrPropertyWithName(
            pThread, pILFrame, pClassValue, valueKind, parts[i], &pResultValue, evalFlags));
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

static std::vector<std::string> ParseType(const std::string &expression, std::vector<int> &ranks)
{
    std::vector<std::string> result;
    int paramDepth = 0;

    result.push_back("");

    for (char c : expression)
    {
        switch(c)
        {
            case '.':
                if (paramDepth == 0)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '[':
                if (paramDepth == 0)
                {
                    ranks.push_back(1);
                    continue;
                }
                break;
            case ']':
                if (paramDepth == 0)
                    continue;
                break;
            case ',':
                if (paramDepth == 0)
                {
                    if (!ranks.empty())
                        ranks.back()++;
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

static std::vector<std::string> ParseGenericParams(const std::string &part, std::string &typeName)
{
    std::vector<std::string> result;

    std::size_t start = part.find('<');
    if (start == std::string::npos)
    {
        typeName = part;
        return result;
    }

    int paramDepth = 0;
    bool inArray = false;

    result.push_back("");

    for (std::size_t i = start; i < part.size(); i++)
    {
        char c = part.at(i);
        switch(c)
        {
            case ',':
                if (paramDepth == 1 && !inArray)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '[':
                inArray = true;
                break;
            case ']':
                inArray = false;
                break;
            case '<':
                paramDepth++;
                if (paramDepth == 1) continue;
                break;
            case '>':
                paramDepth--;
                if (paramDepth == 0) continue;
                break;
            default:
                break;
        }
        result.back() += c;
    }
    typeName = part.substr(0, start) + '`' + std::to_string(result.size());
    return result;
}

static std::vector<std::string> GatherParameters(const std::vector<std::string> &parts, int indexEnd)
{
    std::vector<std::string> result;
    for (int i = 0; i < indexEnd; i++)
    {
        std::string typeName;
        std::vector<std::string> params = ParseGenericParams(parts[i], typeName);
        result.insert(result.end(), params.begin(), params.end());
    }
    return result;
}

static HRESULT FindTypeInModule(ICorDebugModule *pModule, const std::vector<std::string> &parts, int &nextPart, mdTypeDef &typeToken)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::string currentTypeName;

    // Search for type in module
    for (int i = nextPart; i < (int)parts.size(); i++)
    {
        std::string name;
        ParseGenericParams(parts[i], name);
        currentTypeName += (currentTypeName.empty() ? "" : ".") + name;

        typeToken = GetTypeTokenForName(pMD, mdTypeDefNil, currentTypeName);
        if (typeToken != mdTypeDefNil)
        {
            nextPart = i + 1;
            break;
        }
    }

    if (typeToken == mdTypeDefNil) // type not found, continue search in next module
        return E_FAIL;

    // Resolve nested class
    for (int j = nextPart; j < (int)parts.size(); j++)
    {
        std::string name;
        ParseGenericParams(parts[j], name);
        mdTypeDef classToken = GetTypeTokenForName(pMD, typeToken, name);
        if (classToken == mdTypeDefNil)
            break;
        typeToken = classToken;
        nextPart = j + 1;
    }

    return S_OK;
}

HRESULT Evaluator::GetType(
    const std::string &typeName,
    ICorDebugThread *pThread,
    ICorDebugType **ppType)
{
    HRESULT Status;
    std::vector<int> ranks;
    std::vector<std::string> classParts = ParseType(typeName, ranks);
    if (classParts.size() == 1)
        classParts[0] = TypePrinter::RenameToSystem(classParts[0]);

    ToRelease<ICorDebugType> pType;
    int nextClassPart = 0;
    IfFailRet(FindType(classParts, nextClassPart, pThread, nullptr, &pType));

    if (!ranks.empty())
    {
        ToRelease<ICorDebugAppDomain2> pAppDomain2;
        ToRelease<ICorDebugAppDomain> pAppDomain;
        IfFailRet(pThread->GetAppDomain(&pAppDomain));
        IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

        for (auto irank = ranks.rbegin(); irank != ranks.rend(); ++irank)
        {
            ToRelease<ICorDebugType> pElementType(std::move(pType));
            IfFailRet(pAppDomain2->GetArrayOrPointerType(
                *irank > 1 ? ELEMENT_TYPE_ARRAY : ELEMENT_TYPE_SZARRAY,
                *irank,
                pElementType,
                &pType));
        }
    }

    *ppType = pType.Detach();
    return S_OK;
}

HRESULT Evaluator::ResolveParameters(
    const std::vector<std::string> &params,
    ICorDebugThread *pThread,
    std::vector< ToRelease<ICorDebugType> > &types)
{
    HRESULT Status;
    for (auto &p : params)
    {
        ICorDebugType *tmpType;
        IfFailRet(GetType(p, pThread, &tmpType));
        types.emplace_back(tmpType);
    }
    return S_OK;
}

HRESULT Evaluator::FindType(
    const std::vector<std::string> &parts,
    int &nextPart,
    ICorDebugThread *pThread,
    ICorDebugModule *pModule,
    ICorDebugType **ppType,
    ICorDebugModule **ppModule)
{
    HRESULT Status;

    if (pModule)
        pModule->AddRef();
    ToRelease<ICorDebugModule> pTypeModule(pModule);

    mdTypeDef typeToken = mdTypeDefNil;

    if (!pTypeModule)
    {
        m_modules.ForEachModule([&](ICorDebugModule *pModule)->HRESULT {
            if (typeToken != mdTypeDefNil) // already found
                return S_OK;

            if (SUCCEEDED(FindTypeInModule(pModule, parts, nextPart, typeToken)))
            {
                pModule->AddRef();
                pTypeModule = pModule;
            }
            return S_OK;
        });
    }
    else
    {
        FindTypeInModule(pTypeModule, parts, nextPart, typeToken);
    }

    if (typeToken == mdTypeDefNil)
        return E_FAIL;

    std::vector<std::string> params = GatherParameters(parts, nextPart);
    std::vector< ToRelease<ICorDebugType> > types;
    IfFailRet(ResolveParameters(params, pThread, types));

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pTypeModule->GetClassFromToken(typeToken, &pClass));

    ToRelease<ICorDebugClass2> pClass2;
    IfFailRet(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID*) &pClass2));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pTypeModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    DWORD flags;
    ULONG nameLen;
    mdToken tkExtends;
    IfFailRet(pMD->GetTypeDefProps(typeToken, nullptr, 0, &nameLen, &flags, &tkExtends));

    std::list<std::string> args;
    std::string eTypeName;
    IfFailRet(TypePrinter::NameForToken(typeToken, pMD, eTypeName, true, args));

    bool isValueType = eTypeName == "System.ValueType" || eTypeName == "System.Enum";
    CorElementType et = isValueType ? ELEMENT_TYPE_VALUETYPE : ELEMENT_TYPE_CLASS;

    ToRelease<ICorDebugType> pType;
    IfFailRet(pClass2->GetParameterizedType(et, static_cast<uint32_t>(types.size()), (ICorDebugType **)types.data(), &pType));

    *ppType = pType.Detach();
    if (ppModule)
        *ppModule = pTypeModule.Detach();

    return S_OK;
}

HRESULT Evaluator::FollowNested(ICorDebugThread *pThread,
                                ICorDebugILFrame *pILFrame,
                                const std::string &methodClass,
                                const std::vector<std::string> &parts,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    std::vector<int> ranks;
    std::vector<std::string> classParts = ParseType(methodClass, ranks);
    int nextClassPart = 0;

    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(FindType(classParts, nextClassPart, pThread, nullptr, &pType, &pModule));

    while (!classParts.empty())
    {
        ToRelease<ICorDebugType> pEnclosingType(std::move(pType));
        nextClassPart = 0;
        if (FAILED(FindType(classParts, nextClassPart, pThread, pModule, &pType)))
            break;

        ToRelease<ICorDebugValue> pTypeValue;
        IfFailRet(EvalObjectNoConstructor(pThread, pType, &pTypeValue, evalFlags));

        if (SUCCEEDED(FollowFields(pThread, pILFrame, pTypeValue, ValueIsClass, parts, 0, ppResult, evalFlags)))
            return S_OK;

        classParts.pop_back();
    }

    return E_FAIL;
}

HRESULT Evaluator::EvalExpr(ICorDebugThread *pThread,
                            ICorDebugFrame *pFrame,
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

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ToRelease<ICorDebugValue> pResultValue;
    ToRelease<ICorDebugValue> pThisValue;

    if (parts.at(nextPart) == "$exception")
    {
        pThread->GetCurrentException(&pResultValue);
    }
    else
    {
        IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame,
                                            ICorDebugValue *pValue,
                                            const std::string &name) -> HRESULT
        {
            if (pResultValue)
                return S_OK; // TODO: Create a fast way to exit

            if (name == "this" && pValue)
            {
                pValue->AddRef();
                pThisValue = pValue;
            }

            if (name == parts.at(nextPart) && pValue)
            {
                pValue->AddRef();
                pResultValue = pValue;
            }
            return S_OK;
        }));
    }

    // After FollowFields pFrame may be neutered, so get the class name of the method here
    std::string methodClass;
    std::string methodName;
    TypePrinter::GetTypeAndMethod(pFrame, methodClass, methodName);

    if (!pResultValue && pThisValue) // check this.*
    {
        if (SUCCEEDED(FollowFields(pThread, pILFrame, pThisValue, ValueIsVariable, parts, nextPart, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    if (!pResultValue) // check statics in nested classes
    {
        if (SUCCEEDED(FollowNested(pThread, pILFrame, methodClass, parts, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    ValueKind valueKind;;
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
        IfFailRet(FindType(parts, nextPart, pThread, nullptr, &pType));
        IfFailRet(EvalObjectNoConstructor(pThread, pType, &pResultValue, evalFlags));
        valueKind = ValueIsClass;
    }

    IfFailRet(FollowFields(pThread, pILFrame, pResultValue, valueKind, parts, nextPart, &pResultValue, evalFlags));

    *ppResult = pResultValue.Detach();

    return S_OK;
}

HRESULT Evaluator::CreateString(ICorDebugThread *pThread, const std::string &value, ICorDebugValue **ppNewString)
{
    HRESULT Status;
    auto value16t = to_utf16(value);

    ToRelease<ICorDebugEval> pEval;
    IfFailRet(pThread->CreateEval(&pEval));

    IfFailRet(pEval->NewString(value16t.c_str()));

    return WaitEvalResult(pThread, pEval, ppNewString);
}
