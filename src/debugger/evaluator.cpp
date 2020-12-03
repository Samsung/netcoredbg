// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/manageddebugger.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <vector>
#include <list>

#include "utils/utf.h"
#include "metadata/typeprinter.h"
#include "valueprint.h"

namespace netcoredbg
{

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
        m_pRunClassConstructor.Free();
    if (m_pGetTypeHandle)
        m_pGetTypeHandle.Free();
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
            pThread, pILFrame, pClassValue, valueKind, parts[i], &pResultValue, evalFlags));  // NOLINT(clang-analyzer-cplusplus.Move)
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
                &pType));        // NOLINT(clang-analyzer-cplusplus.Move)
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
        if (FAILED(FindType(classParts, nextClassPart, pThread, pModule, &pType)))  // NOLINT(clang-analyzer-cplusplus.Move)
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

    ICorDebugValue *pValue = pResultValue.Detach();
    IfFailRet(FollowFields(pThread, pILFrame, pValue, valueKind, parts, nextPart, &pResultValue, evalFlags));

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

void Evaluator::NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval)
{
    std::lock_guard<std::mutex> lock(m_evalMutex);
    if (!pThread)
    {
        m_evalResults.clear();
        return;
    }

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::unique_ptr< ToRelease<ICorDebugValue> > ppEvalResult(new ToRelease<ICorDebugValue>());
    if (pEval)
    {
        pEval->GetResult(&(*ppEvalResult));
    }

    auto it = m_evalResults.find(threadId);

    if (it == m_evalResults.end())
        return;

    it->second.pEval = pEval;
    it->second.promiseValue.set_value(std::move(ppEvalResult));

    m_evalResults.erase(it);
}

bool Evaluator::IsEvalRunning()
{
    std::lock_guard<std::mutex> lock(m_evalMutex);
    return !m_evalResults.empty();
}

std::future<std::unique_ptr<ToRelease<ICorDebugValue> > > Evaluator::RunEval(
    ICorDebugThread *pThread,
    ICorDebugEval *pEval)
{
    std::promise<std::unique_ptr<ToRelease<ICorDebugValue> > > p;
    auto f = p.get_future();
    if (!f.valid()) {
        LOGE("get_future() returns not valid promise object");
    }

    HRESULT res;
    DWORD threadId = 0;
    pThread->GetID(&threadId);

    ToRelease<ICorDebugProcess> pProcess;
    res = pThread->GetProcess(&pProcess);
    if (FAILED(res)) {
        LOGE("GetProcess() failed, %0x", res);
        return f;
    }

    std::lock_guard<std::mutex> lock(m_evalMutex);
    if (!m_evalResults.insert(std::make_pair(threadId, evalResult_t{pEval, std::move(p)})).second)
        return f; // Already running eval? The future will throw broken promise

    ToRelease<ICorDebugAppDomain> pAppDomainLocal;
    pThread->GetAppDomain(&pAppDomainLocal);
    pAppDomainLocal->SetAllThreadsDebugState(THREAD_SUSPEND, pThread);
    pThread->SetDebugState(THREAD_RUN);

    res = pProcess->Continue(0);
    if (FAILED(res)) {
        LOGE("Continue() failed, %0x", res);
        m_evalResults.erase(threadId);
    }

    return f;
}

ICorDebugEval *Evaluator::FindEvalForThread(ICorDebugThread *pThread)
{
    DWORD threadId = 0;
    if (FAILED(pThread->GetID(&threadId)))
        return nullptr;

    auto it = m_evalResults.find(threadId);
    if (it == m_evalResults.end())
        return nullptr;

    return it->second.pEval;
}

HRESULT Evaluator::WaitEvalResult(ICorDebugThread *pThread,
                                  ICorDebugEval *pEval,
                                  ICorDebugValue **ppEvalResult)
{
    try
    {
        auto f = RunEval(pThread, pEval);

        if (!f.valid())
            return E_FAIL;

        // NOTE
        // MSVS 2017 debugger and newer use config file
        // C:\Program Files (x86)\Microsoft Visual Studio\YYYY\VERSION\Common7\IDE\Profiles\CSharp.vssettings
        // by default NormalEvalTimeout is 5000 milliseconds
        //
        // TODO add timeout configuration feature (care about VSCode, MSVS with Tizen plugin, standalone usage)

        std::future_status timeoutStatus = f.wait_for(std::chrono::milliseconds(5000));
        if (timeoutStatus == std::future_status::timeout)
        {
            LOGW("Evaluation timed out.");

            // NOTE
            // Call ICorDebugEval::Abort() and ICorDebugEval2::RudeAbort() during process execution is allowed, we are safe here.
            // 
            // All CoreCLR releases at least till version 3.1.3, don't have proper x86 implementation for ICorDebugEval::Abort().
            // This issue looks like CoreCLR terminate managed process execution instead of abort evaluation.

            if (FAILED(pEval->Abort()))
            {
                HRESULT Status = S_OK;
                ToRelease<ICorDebugEval2> pEval2;
                IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                IfFailRet(pEval2->RudeAbort());
            }
        }

        auto evalResult = f.get();

        if (!ppEvalResult)
            return S_OK;

        if (!evalResult->GetPtr())
            return E_FAIL;

        *ppEvalResult = evalResult->Detach();
    }
    catch (const std::future_error&)
    {
       return E_FAIL;
    }

    return S_OK;
}

HRESULT Evaluator::EvalFunction(
    ICorDebugThread *pThread,
    ICorDebugFunction *pFunc,
    ICorDebugType *pType, // may be nullptr
    ICorDebugValue *pArgValue, // may be nullptr
    ICorDebugValue **ppEvalResult,
    int evalFlags)
{
    LogFuncEntry();

    if (evalFlags & EVAL_NOFUNCEVAL)
        return S_OK;

    HRESULT Status = S_OK;
    ToRelease<ICorDebugEval> pEval;

    IfFailRet(pThread->CreateEval(&pEval));

    std::vector< ToRelease<ICorDebugType> > typeParams;

    if (pType)
    {
        ToRelease<ICorDebugTypeEnum> pTypeEnum;
        if (SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
        {
            ICorDebugType *curType;
            ULONG fetched = 0;
            while (SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
            {
                typeParams.emplace_back(curType);
            }
        }
    }

    ToRelease<ICorDebugEval2> pEval2;
    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));

    HRESULT res = pEval2->CallParameterizedFunction(
        pFunc,
        static_cast<uint32_t>(typeParams.size()),
        (ICorDebugType **)typeParams.data(),
        pArgValue ? 1 : 0,
        pArgValue ? &pArgValue : nullptr
    );

    switch (res) {
        case CORDBG_E_ILLEGAL_IN_OPTIMIZED_CODE: {
            LOGE("ERROR: Can not evaluate in optimized code");
            return res;
        }
        break;

        case CORDBG_E_APPDOMAIN_MISMATCH: {
            LOGE("ERROR: Object is in wrong AppDomain");
            return res;
        }
        break;

        case CORDBG_E_FUNCTION_NOT_IL: {
            LOGE("ERROR: Function does not have IL code");
            return res;
        }
        break;

        case CORDBG_E_ILLEGAL_IN_STACK_OVERFLOW: {
            LOGE("ERROR: Can not evaluate after stack overflow");
            return res;
        }
        break;

        case CORDBG_E_FUNC_EVAL_BAD_START_POINT: {
            LOGE("ERROR: Func eval cannot work. Bad starting point");
            return res;
        }
        break;

        case CORDBG_E_ILLEGAL_AT_GC_UNSAFE_POINT: {
            LOGE("ERROR: Thread is in GC unsafe point");
            // INFO: Skip this evaluations as unsafe state.
            //  we can continue this thread for change state from unsafe to safe,
            //  but after running thread we can get a new unhanded exception and
            //  as a result in 50/50 terminated process.
            return res;
        }
        break;

        default:
            IfFailRet(res);
        break;
    }


    DWORD tid;
    pThread->GetID(&tid);
    push_eval_queue(ThreadId{tid});


    // TODO: maybe in this point we can _restore_ thread state?
    // fprintf(stderr, "After WaitEvalResult [EvalFunction]\n");
    // And for "continue" we can save mask for threads?
    return WaitEvalResult(pThread, pEval, ppEvalResult);
}

HRESULT Evaluator::EvalObjectNoConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppEvalResult,
    int evalFlags,
    bool suppressFinalize)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugEval> pEval;

    IfFailRet(pThread->CreateEval(&pEval));

    std::vector< ToRelease<ICorDebugType> > typeParams;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));

    ToRelease<ICorDebugTypeEnum> pTypeEnum;
    if (SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ICorDebugType *curType;
        ULONG fetched = 0;
        while (SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
        {
            typeParams.emplace_back(curType);
        }
    }

    ToRelease<ICorDebugEval2> pEval2;
    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));

    IfFailRet(pEval2->NewParameterizedObjectNoConstructor(
        pClass,
        static_cast<uint32_t>(typeParams.size()),
        (ICorDebugType **)typeParams.data()
    ));

    IfFailRet(WaitEvalResult(pThread, pEval, ppEvalResult));

    if (suppressFinalize)
    {
        if (!m_pSuppressFinalize)
        {
            ToRelease<ICorDebugModule> pModule;
            IfFailRet(m_modules.GetModuleWithName("System.Private.CoreLib.dll", &pModule));

            static const WCHAR gcName[] = W("System.GC");
            static const WCHAR suppressFinalizeMethodName[] = W("SuppressFinalize");
            IfFailRet(FindFunction(pModule, gcName, suppressFinalizeMethodName, &m_pSuppressFinalize));
        }

        if (!m_pSuppressFinalize)
            return E_FAIL;

        IfFailRet(EvalFunction(pThread, m_pSuppressFinalize, nullptr, *ppEvalResult, nullptr /* void method */, evalFlags));
    }

    return S_OK;
}

static HRESULT FindMethod(ICorDebugType *pType, const WCHAR *methodName, ICorDebugFunction **ppFunc)
{
    HRESULT Status;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));

    mdTypeDef currentTypeDef;
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numMethods = 0;
    HCORENUM hEnum = NULL;
    mdMethodDef methodDef = mdMethodDefNil;

    pMD->EnumMethodsWithName(&hEnum, currentTypeDef, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(hEnum);

    if (numMethods == 1)
        return pModule->GetFunctionFromToken(methodDef, ppFunc);

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;

    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return E_FAIL;
        else if (baseTypeName != "System.Object" && baseTypeName != "System.ValueType")
        {
            return FindMethod(pBaseType, methodName, ppFunc);
        }
    }

    return E_FAIL;
}

HRESULT Evaluator::getObjectByFunction(
    const std::string &func,
    ICorDebugThread *pThread,
    ICorDebugValue *pInValue,
    ICorDebugValue **ppOutValue,
    int evalFlags)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;

    IfFailRet(pInValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *)&pValue2));
    IfFailRet(pValue2->GetExactType(&pType));
    ToRelease<ICorDebugFunction> pFunc;

    auto methodName = to_utf16(func);
    IfFailRet(FindMethod(pType, methodName.c_str(), &pFunc));

    return EvalFunction(pThread, pFunc, pType, pInValue, ppOutValue, evalFlags);
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

HRESULT Evaluator::GetLiteralValue(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugModule *pModule,
    PCCOR_SIGNATURE pSignatureBlob,
    ULONG sigBlobLength,
    UVCP_CONSTANT pRawValue,
    ULONG rawValueLength,
    ICorDebugValue **ppLiteralValue)
{
    HRESULT Status = S_OK;

    CorSigUncompressCallingConv(pSignatureBlob);
    CorElementType underlyingType;
    CorSigUncompressElementType(pSignatureBlob, &underlyingType);

    if (!pRawValue || !pThread)
        return S_FALSE;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ToRelease<ICorDebugEval> pEval;
    IfFailRet(pThread->CreateEval(&pEval));

    switch(underlyingType)
    {
        case ELEMENT_TYPE_OBJECT:
            IfFailRet(pEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, ppLiteralValue));
            break;
        case ELEMENT_TYPE_CLASS:
        {
            // Get token and create null reference
            mdTypeDef tk;
            CorSigUncompressElementType(pSignatureBlob);
            CorSigUncompressToken(pSignatureBlob, &tk);

            ToRelease<ICorDebugClass> pValueClass;
            IfFailRet(pModule->GetClassFromToken(tk, &pValueClass));

            IfFailRet(pEval->CreateValue(ELEMENT_TYPE_CLASS, pValueClass, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_ARRAY:
        case ELEMENT_TYPE_SZARRAY:
        {
            // Get type name from signature and get its ICorDebugType
            std::string typeName;
            TypePrinter::NameForTypeSig(pSignatureBlob, pType, pMD, typeName);
            ToRelease<ICorDebugType> pElementType;
            IfFailRet(GetType(typeName, pThread, &pElementType));

            ToRelease<ICorDebugAppDomain2> pAppDomain2;
            ToRelease<ICorDebugAppDomain> pAppDomain;
            IfFailRet(pThread->GetAppDomain(&pAppDomain));
            IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

            // We can not directly create null value of specific array type.
            // Instead, we create one element array with element type set to our specific array type.
            // Since array elements are initialized to null, we get our null value from the first array item.

            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));

            ULONG32 dims = 1;
            ULONG32 bounds = 0;
            IfFailRet(pEval2->NewParameterizedArray(pElementType, 1, &dims, &bounds));
            ToRelease<ICorDebugValue> pTmpArrayValue;
            IfFailRet(WaitEvalResult(pThread, pEval, &pTmpArrayValue));

            BOOL isNull = FALSE;
            ToRelease<ICorDebugValue> pUnboxedResult;
            IfFailRet(DereferenceAndUnboxValue(pTmpArrayValue, &pUnboxedResult, &isNull));

            ToRelease<ICorDebugArrayValue> pArray;
            IfFailRet(pUnboxedResult->QueryInterface(IID_ICorDebugArrayValue, (LPVOID*) &pArray));
            IfFailRet(pArray->GetElementAtPosition(0, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_GENERICINST:
        {
            // Get type name from signature and get its ICorDebugType
            std::string typeName;
            TypePrinter::NameForTypeSig(pSignatureBlob, pType, pMD, typeName);
            ToRelease<ICorDebugType> pValueType;
            IfFailRet(GetType(typeName, pThread, &pValueType));

            // Create value from ICorDebugType
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->CreateValueForType(pValueType, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_VALUETYPE:
        {
            // Get type token
            mdTypeDef tk;
            CorSigUncompressElementType(pSignatureBlob);
            CorSigUncompressToken(pSignatureBlob, &tk);

            ToRelease<ICorDebugClass> pValueClass;
            IfFailRet(pModule->GetClassFromToken(tk, &pValueClass));

            // Create value (without calling a constructor)
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->NewParameterizedObjectNoConstructor(pValueClass, 0, nullptr));

            ToRelease<ICorDebugValue> pValue;
            IfFailRet(WaitEvalResult(pThread, pEval, &pValue));

            // Set value
            BOOL isNull = FALSE;
            ToRelease<ICorDebugValue> pEditableValue;
            IfFailRet(DereferenceAndUnboxValue(pValue, &pEditableValue, &isNull));

            ToRelease<ICorDebugGenericValue> pGenericValue;
            IfFailRet(pEditableValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            IfFailRet(pGenericValue->SetValue((LPVOID)pRawValue));
            *ppLiteralValue = pValue.Detach();
            break;
        }
        case ELEMENT_TYPE_STRING:
        {
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->NewStringWithLength((LPCWSTR)pRawValue, rawValueLength));

            IfFailRet(WaitEvalResult(pThread, pEval, ppLiteralValue));
            break;
        }
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
        {
            ToRelease<ICorDebugValue> pValue;
            IfFailRet(pEval->CreateValue(underlyingType, nullptr, &pValue));
            ToRelease<ICorDebugGenericValue> pGenericValue;
            IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            IfFailRet(pGenericValue->SetValue((LPVOID)pRawValue));
            *ppLiteralValue = pValue.Detach();
            break;
        }
        default:
            return E_FAIL;
    }
    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pInputValue,
    ICorDebugThread *pThread,
    ICorDebugILFrame *pILFrame,
    ICorDebugType *pTypeCast,
    WalkMembersCallback cb)
{
    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull && !pValue.GetPtr()) return S_OK;

    CorElementType inputCorType;
    IfFailRet(pInputValue->GetType(&inputCorType));
    if (inputCorType == ELEMENT_TYPE_PTR)
    {
        return cb(mdMethodDefNil, nullptr, nullptr, pValue, false, "");
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
            ToRelease<ICorDebugValue> pElementValue;
            pArrayValue->GetElementAtPosition(i, &pElementValue);
            IfFailRet(cb(mdMethodDefNil, nullptr, nullptr, pElementValue, false, "[" + IndiciesToStr(ind, base) + "]"));
            IncIndicies(ind, dims);
        }

        return S_OK;
    }

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    if(pTypeCast == nullptr)
        IfFailRet(pValue2->GetExactType(&pType));
    else
    {
        pType = pTypeCast;
        pType->AddRef();
    }

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
        return S_OK;

    IfFailRet(pType->GetClass(&pClass));
    IfFailRet(pClass->GetModule(&pModule));
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::string className;
    TypePrinter::GetTypeOfValue(pType, className);
    if (className == "decimal") // TODO: implement mechanism for walking over custom type fields
        return S_OK;

    std::unordered_set<std::string> backedProperties;

    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&hEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        PCCOR_SIGNATURE pSignatureBlob = nullptr;
        ULONG sigBlobLength = 0;
        UVCP_CONSTANT pRawValue = nullptr;
        ULONG rawValueLength = 0;
        if (SUCCEEDED(pMD->GetFieldProps(fieldDef,
                                         nullptr,
                                         mdName,
                                         _countof(mdName),
                                         &nameLen,
                                         &fieldAttr,
                                         &pSignatureBlob,
                                         &sigBlobLength,
                                         nullptr,
                                         &pRawValue,
                                         &rawValueLength)))
        {
            std::string name = to_utf8(mdName /*, nameLen*/);

            bool is_static = (fieldAttr & fdStatic);

            ToRelease<ICorDebugValue> pFieldVal;

            if(fieldAttr & fdLiteral)
            {
                Status = GetLiteralValue(pThread, pType, pModule, pSignatureBlob, sigBlobLength, pRawValue, rawValueLength, &pFieldVal);
                if (FAILED(Status))
                {
                    pMD->CloseEnum(hEnum);
                    return Status;
                }
            }
            else if (fieldAttr & fdStatic)
            {
                if (pILFrame)
                    pType->GetStaticFieldValue(fieldDef, pILFrame, &pFieldVal);
            }
            else
            {
                ToRelease<ICorDebugObjectValue> pObjValue;
                if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue)))
                    pObjValue->GetFieldValue(pClass, fieldDef, &pFieldVal);
            }

            if (pFieldVal != NULL)
            {
                if (name[0] == '<')
                {
                    size_t endOffset = name.rfind('>');
                    name = name.substr(1, endOffset - 1);
                    backedProperties.insert(name);
                }
            }
            else
            {
                // no need for backing field when we can not get its value
                if (name[0] == '<')
                    continue;
            }

            if (isNull && !is_static)
                continue;
            Status = cb(mdMethodDefNil, pModule, pType, pFieldVal, is_static, name);
            if (FAILED(Status))
            {
                pMD->CloseEnum(hEnum);
                return Status;
            }
        }
    }
    pMD->CloseEnum(hEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, currentTypeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdTypeDef  propertyClass;

        ULONG propertyNameLen = 0;
        UVCP_CONSTANT pDefaultValue;
        ULONG cchDefaultValue;
        mdMethodDef mdGetter;
        WCHAR propertyName[mdNameLen] = W("\0");
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef,
                                            &propertyClass,
                                            propertyName,
                                            _countof(propertyName),
                                            &propertyNameLen,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            &pDefaultValue,
                                            &cchDefaultValue,
                                            nullptr,
                                            &mdGetter,
                                            nullptr,
                                            0,
                                            nullptr)))
        {
            DWORD getterAttr = 0;
            if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
                continue;

            std::string name = to_utf8(propertyName/*, propertyNameLen*/);

            if (backedProperties.find(name) != backedProperties.end())
                continue;

            bool is_static = (getterAttr & mdStatic);

            if (isNull && !is_static)
                continue;

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
            hEnum = NULL;
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
                std::list<std::string> emptyArgs;
                if (FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, emptyArgs)))
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
              continue;

            IfFailRet(cb(mdGetter, pModule, pType, nullptr, is_static, name));
        }
    }
    pMD->CloseEnum(propEnum);

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if(baseTypeName != "System.Object"  && baseTypeName != "System.ValueType")
        {
            // Add fields of base class
            IfFailRet(WalkMembers(pInputValue, pThread, pILFrame, pBaseType, cb));
        }
    }

    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pValue,
    ICorDebugThread *pThread,
    ICorDebugILFrame *pILFrame,
    WalkMembersCallback cb)
{
    return WalkMembers(pValue, pThread, pILFrame, nullptr, cb);
}

static bool has_prefix(const std::string &s, const std::string &prefix)
{
    return prefix.length() <= s.length() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

HRESULT Evaluator::HandleSpecialLocalVar(
    const std::string &localName,
    ICorDebugValue *pLocalValue,
    ICorDebugILFrame *pILFrame,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    static const std::string captureName = "CS$<>";

    HRESULT Status;

    if (!has_prefix(localName, captureName))
        return S_FALSE;

    // Substitute local value with its fields
    IfFailRet(WalkMembers(pLocalValue, nullptr, pILFrame, [&](
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &name)
    {
        if (is_static)
            return S_OK;
        if (has_prefix(name, captureName))
            return S_OK;
        if (!locals.insert(name).second)
            return S_OK; // already in the list
        return cb(pILFrame, pValue, name.empty() ? "this" : name);
    }));

    return S_OK;
}

HRESULT Evaluator::HandleSpecialThisParam(
    ICorDebugValue *pThisValue,
    ICorDebugILFrame *pILFrame,
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
    IfFailRet(WalkMembers(pThisValue, nullptr, pILFrame, [&](
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &name)
    {
        HRESULT Status;
        if (is_static)
            return S_OK;
        IfFailRet(HandleSpecialLocalVar(name, pValue, pILFrame, locals, cb));
        if (Status == S_OK)
            return S_OK;
        locals.insert(name);
        return cb(pILFrame, pValue, name.empty() ? "this" : name);
    }));
    return S_OK;
}

HRESULT Evaluator::WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb)
{
    HRESULT Status;

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunction->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdMethodDef methodDef;
    IfFailRet(pFunction->GetToken(&methodDef));

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
            Status = pParamEnum->Next(1, &pValue, &cArgsFetched);

            if (FAILED(Status))
                continue;

            if (Status == S_FALSE)
                break;

            if (thisParam)
            {
                IfFailRet(HandleSpecialThisParam(pValue, pILFrame, locals, cb));
                if (Status == S_OK)
                    continue;
            }

            locals.insert(paramName);
            IfFailRet(cb(pILFrame, pValue, paramName));
        }
    }

    ULONG cLocals = 0;
    ToRelease<ICorDebugValueEnum> pLocalsEnum;

    ULONG32 currentIlOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&currentIlOffset, &mappingResult));

    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));
    IfFailRet(pLocalsEnum->GetCount(&cLocals));
    if (cLocals > 0)
    {
        for (ULONG i = 0; i < cLocals; i++)
        {
            std::string paramName;

            ToRelease<ICorDebugValue> pValue;
            ULONG32 ilStart;
            ULONG32 ilEnd;
            Status = m_modules.GetFrameNamedLocalVariable(pModule, pILFrame, methodDef, i, paramName, &pValue, &ilStart, &ilEnd);

            if (FAILED(Status))
                continue;

            if (currentIlOffset < ilStart || currentIlOffset >= ilEnd)
                continue;

            if (Status == S_FALSE)
                break;

            IfFailRet(HandleSpecialLocalVar(paramName, pValue, pILFrame, locals, cb));
            if (Status == S_OK)
                continue;

            locals.insert(paramName);
            IfFailRet(cb(pILFrame, pValue, paramName));
        }
    }

    return S_OK;
}

} // namespace netcoredbg
