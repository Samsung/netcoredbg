// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <vector>
#include <algorithm>
#include "debugger/evalhelpers.h"
#include "debugger/evalwaiter.h"
#include "debugger/evalutils.h"
#include "utils/utf.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "valueprint.h"

namespace netcoredbg
{

void EvalHelpers::Cleanup()
{
    m_pSuppressFinalizeMutex.lock();
    if (m_pSuppressFinalize)
        m_pSuppressFinalize.Free();
    m_pSuppressFinalizeMutex.unlock();

    m_typeObjectCacheMutex.lock();
    m_typeObjectCache.clear();
    m_typeObjectCacheMutex.unlock();
}

HRESULT EvalHelpers::CreateString(ICorDebugThread *pThread, const std::string &value, ICorDebugValue **ppNewString)
{
    auto value16t = to_utf16(value);
    return m_sharedEvalWaiter->WaitEvalResult(pThread, ppNewString,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            HRESULT Status;
            IfFailRet(pEval->NewString(value16t.c_str()));
            return S_OK;
        });
}

// Call managed function in debuggee process.
// [in] pThread - managed thread for evaluation;
// [in] pFunc - function to call;
// [in] ppArgsType - pointer to args Type array, could be nullptr;
// [in] ArgsTypeCount - size of args Type array;
// [in] ppArgsValue - pointer to args Value array, could be nullptr;
// [in] ArgsValueCount - size of args Value array;
// [out] ppEvalResult - return value;
// [in] evalFlags - evaluation flags.
HRESULT EvalHelpers::EvalFunction(
    ICorDebugThread *pThread,
    ICorDebugFunction *pFunc,
    ICorDebugType **ppArgsType,
    ULONG32 ArgsTypeCount,
    ICorDebugValue **ppArgsValue,
    ULONG32 ArgsValueCount,
    ICorDebugValue **ppEvalResult,
    int evalFlags)
{
    assert((!ppArgsType && ArgsTypeCount == 0) || (ppArgsType && ArgsTypeCount > 0));
    assert((!ppArgsValue && ArgsValueCount == 0) || (ppArgsValue && ArgsValueCount > 0));

    if (evalFlags & EVAL_NOFUNCEVAL)
        return S_OK;

    std::vector< ToRelease<ICorDebugType> > typeParams;
    // Reserve memory from the beginning, since typeParams will have ArgsTypeCount or more count of elements for sure.
    typeParams.reserve(ArgsTypeCount);

    for (ULONG32 i = 0; i < ArgsTypeCount; i++)
    {
        ToRelease<ICorDebugTypeEnum> pTypeEnum;
        if (SUCCEEDED(ppArgsType[i]->EnumerateTypeParameters(&pTypeEnum)))
        {
            ICorDebugType *curType;
            ULONG fetched = 0;
            while (SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
            {
                typeParams.emplace_back(curType);
            }
        }
    }

    return m_sharedEvalWaiter->WaitEvalResult(pThread, ppEvalResult,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            HRESULT Status;
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->CallParameterizedFunction(
                pFunc,
                static_cast<uint32_t>(typeParams.size()),
                (ICorDebugType **)typeParams.data(),
                ArgsValueCount,
                ppArgsValue));
            return S_OK;
        });
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

static HRESULT FindFunction(ICorDebugModule *pModule,
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

static bool TypeHaveStaticMembers(ICorDebugType *pType)
{
    HRESULT Status;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&hEnum, typeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        DWORD fieldAttr = 0;
        if (FAILED(pMD->GetFieldProps(fieldDef, nullptr, nullptr, 0, nullptr, &fieldAttr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr)))
        {
            continue;
        }

        if (fieldAttr & fdStatic)
        {
            pMD->CloseEnum(hEnum);
            return true;
        }
    }
    pMD->CloseEnum(hEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdMethodDef mdGetter;
        if (FAILED(pMD->GetPropertyProps(propertyDef, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, &mdGetter, nullptr, 0, nullptr)))
        {
            continue;
        }

        DWORD getterAttr = 0;
        if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
        {
            continue;
        }

        if (getterAttr & mdStatic)
        {
            pMD->CloseEnum(propEnum);
            return true;
        }
    }
    pMD->CloseEnum(propEnum);

    return false;
}

HRESULT EvalHelpers::TryReuseTypeObjectFromCache(ICorDebugType *pType, ICorDebugValue **ppTypeObjectResult)
{
    std::lock_guard<std::mutex> lock(m_typeObjectCacheMutex);

    HRESULT Status;
    ToRelease<ICorDebugType2> iCorType2;
    IfFailRet(pType->QueryInterface(IID_ICorDebugType2, (LPVOID*) &iCorType2));

    COR_TYPEID typeID;
    IfFailRet(iCorType2->GetTypeID(&typeID));

    auto is_same = [&typeID](type_object_t &typeObject){ return typeObject.id.token1 == typeID.token1 && typeObject.id.token2 == typeID.token2; };
    auto it = std::find_if(m_typeObjectCache.begin(), m_typeObjectCache.end(), is_same);
    if (it == m_typeObjectCache.end())
        return E_FAIL;

    // Move data to begin, so, last used will be on front.
    if (it != m_typeObjectCache.begin())
        m_typeObjectCache.splice(m_typeObjectCache.begin(), m_typeObjectCache, it);

    if (ppTypeObjectResult)
    {
        // We don't check handle's status here, since we store only strong handles.
        // https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/cordebughandletype-enumeration
        // The handle is strong, which prevents an object from being reclaimed by garbage collection.
        return m_typeObjectCache.front().typeObject->QueryInterface(IID_ICorDebugValue, (LPVOID *)ppTypeObjectResult);
    }

    return S_OK;
}

HRESULT EvalHelpers::AddTypeObjectToCache(ICorDebugType *pType, ICorDebugValue *pTypeObject)
{
    std::lock_guard<std::mutex> lock(m_typeObjectCacheMutex);

    HRESULT Status;
    ToRelease<ICorDebugType2> iCorType2;
    IfFailRet(pType->QueryInterface(IID_ICorDebugType2, (LPVOID*) &iCorType2));

    COR_TYPEID typeID;
    IfFailRet(iCorType2->GetTypeID(&typeID));

    auto is_same = [&typeID](type_object_t &typeObject){ return typeObject.id.token1 == typeID.token1 && typeObject.id.token2 == typeID.token2; };
    auto it = std::find_if(m_typeObjectCache.begin(), m_typeObjectCache.end(), is_same);
    if (it != m_typeObjectCache.end())
        return S_OK;

    ToRelease<ICorDebugHandleValue> iCorHandleValue;
    IfFailRet(pTypeObject->QueryInterface(IID_ICorDebugHandleValue, (LPVOID *) &iCorHandleValue));

    CorDebugHandleType handleType;
    if (FAILED(iCorHandleValue->GetHandleType(&handleType)) ||
        handleType != HANDLE_STRONG)
        return E_FAIL;

    if (m_typeObjectCache.size() == m_typeObjectCacheSize)
    {
        // Re-use last list entry.
        m_typeObjectCache.back().id = typeID;
        m_typeObjectCache.back().typeObject.Free();
        m_typeObjectCache.back().typeObject = iCorHandleValue.Detach();
        assert(m_typeObjectCacheSize >= 2);
        m_typeObjectCache.splice(m_typeObjectCache.begin(), m_typeObjectCache, std::prev(m_typeObjectCache.end()));
    }
    else
        m_typeObjectCache.emplace_front(type_object_t{typeID, iCorHandleValue.Detach()});

    return S_OK;
}

HRESULT EvalHelpers::CreatTypeObjectStaticConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppTypeObjectResult,
    bool DetectStaticMembers)
{
    HRESULT Status;

    CorElementType et;
    IfFailRet(pType->GetType(&et));

    if (et != ELEMENT_TYPE_CLASS && et != ELEMENT_TYPE_VALUETYPE)
        return S_OK;

    // Check cache first, before check type for static members.
    if (SUCCEEDED(TryReuseTypeObjectFromCache(pType, ppTypeObjectResult)))
        return S_OK;

    // Create type object only in case type have static members.
    // Note, for some cases we have static members check outside this method.
    if (DetectStaticMembers && !TypeHaveStaticMembers(pType))
        return S_FALSE;

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

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));

    ToRelease<ICorDebugValue> pTypeObject;
    IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pTypeObject,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->NewParameterizedObjectNoConstructor(
                pClass,
                static_cast<uint32_t>(typeParams.size()),
                (ICorDebugType **)typeParams.data()
            ));
            return S_OK;
        }));

    if (et == ELEMENT_TYPE_CLASS)
    {
        std::lock_guard<std::mutex> lock(m_pSuppressFinalizeMutex);

        if (!m_pSuppressFinalize)
        {
            ToRelease<ICorDebugModule> pModule;
            IfFailRet(m_sharedModules->GetModuleWithName("System.Private.CoreLib.dll", &pModule));

            static const WCHAR gcName[] = W("System.GC");
            static const WCHAR suppressFinalizeMethodName[] = W("SuppressFinalize");
            IfFailRet(FindFunction(pModule, gcName, suppressFinalizeMethodName, &m_pSuppressFinalize));
        }

        if (!m_pSuppressFinalize)
            return E_FAIL;

        // Note, this call must ignore any eval flags.
        IfFailRet(EvalFunction(pThread, m_pSuppressFinalize, &pType, 1, pTypeObject.GetRef(), 1, nullptr, defaultEvalFlags));
    }

    AddTypeObjectToCache(pType, pTypeObject);

    if (ppTypeObjectResult)
        *ppTypeObjectResult = pTypeObject.Detach();

    return S_OK;
}

HRESULT EvalHelpers::GetLiteralValue(
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

    if (!pRawValue || !pThread)
        return S_FALSE;

    CorSigUncompressCallingConv(pSignatureBlob);
    CorElementType underlyingType;
    CorSigUncompressElementType(pSignatureBlob, &underlyingType);

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    switch(underlyingType)
    {
        case ELEMENT_TYPE_OBJECT:
        {
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
            IfFailRet(pEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_CLASS:
        {
            // Get token and create null reference
            mdTypeDef tk;
            CorSigUncompressElementType(pSignatureBlob);
            CorSigUncompressToken(pSignatureBlob, &tk);

            ToRelease<ICorDebugClass> pValueClass;
            IfFailRet(pModule->GetClassFromToken(tk, &pValueClass));

            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
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
            IfFailRet(EvalUtils::GetType(typeName, pThread, m_sharedModules.get(), &pElementType));

            ToRelease<ICorDebugAppDomain> pAppDomain;
            IfFailRet(pThread->GetAppDomain(&pAppDomain));
            ToRelease<ICorDebugAppDomain2> pAppDomain2;
            IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

            // We can not directly create null value of specific array type.
            // Instead, we create one element array with element type set to our specific array type.
            // Since array elements are initialized to null, we get our null value from the first array item.

            ULONG32 dims = 1;
            ULONG32 bounds = 0;
            ToRelease<ICorDebugValue> pTmpArrayValue;
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pTmpArrayValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewParameterizedArray(pElementType, 1, &dims, &bounds));
                    return S_OK;
                }));

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
            IfFailRet(EvalUtils::GetType(typeName, pThread, m_sharedModules.get(), &pValueType));

            // Create value from ICorDebugType
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
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
            ToRelease<ICorDebugValue> pValue;
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewParameterizedObjectNoConstructor(pValueClass, 0, nullptr));
                    return S_OK;
                }));

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
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, ppLiteralValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewStringWithLength((LPCWSTR)pRawValue, rawValueLength));
                    return S_OK;
                }));

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
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
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

} // namespace netcoredbg
