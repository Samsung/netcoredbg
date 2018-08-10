// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "manageddebugger.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <vector>
#include <list>

#include "cputil.h"
#include "typeprinter.h"
#include "valueprint.h"


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

    it->second.set_value(std::move(ppEvalResult));

    m_evalResults.erase(it);
}

bool Evaluator::IsEvalRunning()
{
    std::lock_guard<std::mutex> lock(m_evalMutex);
    return !m_evalResults.empty();
}

std::future< std::unique_ptr<ToRelease<ICorDebugValue>> > Evaluator::RunEval(
    ICorDebugThread *pThread,
    ICorDebugEval *pEval)
{
    std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > p;
    auto f = p.get_future();

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    ToRelease<ICorDebugProcess> pProcess;
    if (FAILED(pThread->GetProcess(&pProcess)))
        return f;

    std::lock_guard<std::mutex> lock(m_evalMutex);

    if (!m_evalResults.insert(std::make_pair(threadId, std::move(p))).second)
        return f; // Already running eval? The future will throw broken promise

    if (FAILED(pProcess->Continue(0)))
        m_evalResults.erase(threadId);

    return f;
}

HRESULT Evaluator::WaitEvalResult(ICorDebugThread *pThread,
                                  ICorDebugEval *pEval,
                                  ICorDebugValue **ppEvalResult)
{
    try
    {
        auto evalResult = RunEval(pThread, pEval).get();
        if (!ppEvalResult)
            return S_OK;

        if (!evalResult->GetPtr())
            return E_FAIL;
        *ppEvalResult = evalResult->Detach();
    }
    catch (const std::future_error& e)
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
    ICorDebugValue **ppEvalResult)
{
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

    IfFailRet(pEval2->CallParameterizedFunction(
        pFunc,
        typeParams.size(),
        (ICorDebugType **)typeParams.data(),
        pArgValue ? 1 : 0,
        pArgValue ? &pArgValue : nullptr
    ));

    return WaitEvalResult(pThread, pEval, ppEvalResult);
}

HRESULT Evaluator::EvalObjectNoConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppEvalResult,
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
        typeParams.size(),
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

        IfFailRet(EvalFunction(pThread, m_pSuppressFinalize, nullptr, *ppEvalResult, nullptr /* void method */));
    }

    return S_OK;
}

static HRESULT FindMethod(ICorDebugType *pType, WCHAR *methodName, ICorDebugFunction **ppFunc)
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
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef = mdMethodDefNil;

    pMD->EnumMethodsWithName(&fEnum, currentTypeDef, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(fEnum);

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

HRESULT Evaluator::ObjectToString(
    ICorDebugThread *pThread,
    ICorDebugValue *pValue,
    std::function<void(const std::string&)> cb
)
{
    HRESULT Status = S_OK;

    // Get ToString method token

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;

    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    IfFailRet(pValue2->GetExactType(&pType));

    ToRelease<ICorDebugFunction> pFunc;
    WCHAR methodName[] = W("ToString\0");
    IfFailRet(FindMethod(pType, methodName, &pFunc));

    // Get function object by token and setup the call

    ToRelease<ICorDebugEval> pEval;

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
    IfFailRet(pThread->CreateEval(&pEval));

    std::vector< ToRelease<ICorDebugType> > typeParams;

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

    IfFailRet(pEval2->CallParameterizedFunction(
        pFunc,
        typeParams.size(),
        (ICorDebugType **)typeParams.data(),
        1,
        &pValue
    ));

    std::future< std::unique_ptr<ToRelease<ICorDebugValue>> > evalResult = RunEval(pThread, pEval);

    std::thread(std::bind([=](std::future< std::unique_ptr<ToRelease<ICorDebugValue>> >& f)
    {
        std::string output;
        try
        {
            bool escape = false;

            auto result = f.get();
            if (result->GetPtr())
                PrintValue(result->GetPtr(), output, escape);
        }
        catch (const std::future_error& e)
        {
        }
        cb(output);
    }, std::move(evalResult))).detach();

    return S_OK;
}

static void IncIndicies(std::vector<ULONG32> &ind, const std::vector<ULONG32> &dims)
{
    int i = ind.size() - 1;

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

            // We can not direcly create null value of specific array type.
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
            pEval2->NewStringWithLength((LPCWSTR)pRawValue, rawValueLength);
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

    std::unordered_set<std::string> backedProperies;

    ULONG numFields = 0;
    HCORENUM fEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
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
                IfFailRet(GetLiteralValue(
                    pThread, pType, pModule, pSignatureBlob, sigBlobLength, pRawValue, rawValueLength, &pFieldVal));
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
                    backedProperies.insert(name);
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
            IfFailRet(cb(mdMethodDefNil, pModule, pType, pFieldVal, is_static, name));
        }
    }
    pMD->CloseEnum(fEnum);

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

            if (backedProperies.find(name) != backedProperies.end())
                continue;

            bool is_static = (getterAttr & mdStatic);

            if (isNull && !is_static)
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

HRESULT Evaluator::HandleSpecialLocalVar(
    const std::string &localName,
    ICorDebugValue *pLocalValue,
    ICorDebugILFrame *pILFrame,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    static const std::string captureName = "CS$<>";

    HRESULT Status;

    if (!std::equal(captureName.begin(), captureName.end(), localName.begin()))
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
        HRESULT Status;
        if (is_static)
            return S_OK;
        if (std::equal(captureName.begin(), captureName.end(), name.begin()))
            return S_OK;
        if (!locals.insert(name).second)
            return S_OK; // already in the list
        return cb(pILFrame, pValue, name);
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
    static const std::string hideClass = "<>c";

    HRESULT Status;

    std::string typeName;
    TypePrinter::GetTypeOfValue(pThisValue, typeName);

    std::size_t start = typeName.find_last_of('.');
    if (start == std::string::npos)
        return S_FALSE;

    typeName = typeName.substr(start + 1);

    if (!std::equal(hideClass.begin(), hideClass.end(), typeName.begin()))
        return S_FALSE;

    if (!std::equal(displayClass.begin(), displayClass.end(), typeName.begin()))
        return S_OK; // just do not show this value

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
