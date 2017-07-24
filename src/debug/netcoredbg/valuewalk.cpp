#include "common.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <iomanip>

#include "cputil.h"
#include "modules.h"
#include "typeprinter.h"
#include "valuewalk.h"


// Expr
HRESULT GetType(const std::string &typeName, ICorDebugThread *pThread, ICorDebugType **ppType);

// Valueprint
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = NULL);

typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

static std::mutex g_evalMutex;
static std::condition_variable g_evalCV;
static bool g_evalComplete = false;

void NotifyEvalComplete()
{
    std::lock_guard<std::mutex> lock(g_evalMutex);
    g_evalComplete = true;
    g_evalMutex.unlock();
    g_evalCV.notify_one();
}

static HRESULT WaitEvalResult(ICorDebugProcess *pProcess,
                              ICorDebugEval *pEval,
                              ICorDebugValue **ppEvalResult)
{
    HRESULT Status;
    std::unique_lock<std::mutex> lock(g_evalMutex);
    g_evalComplete = false;
    IfFailRet(pProcess->Continue(0));
    g_evalCV.wait(lock, []{return g_evalComplete;});

    return pEval->GetResult(ppEvalResult);
}

HRESULT EvalFunction(
    ICorDebugThread *pThread,
    ICorDebugFunction *pFunc,
    ICorDebugType *pType, // may be nullptr
    ICorDebugValue *pArgValue, // may be nullptr
    ICorDebugValue **ppEvalResult)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugEval> pEval;

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
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

    return WaitEvalResult(pProcess, pEval, ppEvalResult);
}

HRESULT EvalObjectNoConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppEvalResult)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugEval> pEval;

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
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

    return WaitEvalResult(pProcess, pEval, ppEvalResult);
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

    std::stringstream ss;
    const char *sep = "";
    for (size_t i = 0; i < ind_size; ++i)
    {
        ss << sep;
        sep = ", ";
        ss << (base[i] + ind[i]);
    }
    return ss.str();
}

static HRESULT GetLiteralValue(ICorDebugThread *pThread,
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
            ToRelease<ICorDebugProcess> pProcess;
            IfFailRet(pThread->GetProcess(&pProcess));
            ToRelease<ICorDebugValue> pTmpArrayValue;
            IfFailRet(WaitEvalResult(pProcess, pEval, &pTmpArrayValue));

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
            ToRelease<ICorDebugProcess> pProcess;
            IfFailRet(pThread->GetProcess(&pProcess));
            IfFailRet(WaitEvalResult(pProcess, pEval, &pValue));

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
            ToRelease<ICorDebugProcess> pProcess;
            IfFailRet(pThread->GetProcess(&pProcess));
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            pEval2->NewStringWithLength((LPCWSTR)pRawValue, rawValueLength);
            IfFailRet(WaitEvalResult(pProcess, pEval, ppLiteralValue));
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

static HRESULT WalkMembers(ICorDebugValue *pInputValue,
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
    if(pTypeCast == NULL)
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

HRESULT WalkMembers(ICorDebugValue *pValue,
                    ICorDebugThread *pThread,
                    ICorDebugILFrame *pILFrame,
                    WalkMembersCallback cb)
{
    return WalkMembers(pValue, pThread, pILFrame, nullptr, cb);
}

static HRESULT HandleSpecialLocalVar(const std::string &localName,
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

static HRESULT HandleSpecialThisParam(ICorDebugValue *pThisValue,
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

HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb)
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
            WCHAR wParamName[mdNameLen] = W("\0");

            bool thisParam = i == 0 && (methodAttr & mdStatic) == 0;
            if (thisParam)
                swprintf_s(wParamName, mdNameLen, W("this\0"));
            else
            {
                int idx = ((methodAttr & mdStatic) == 0)? i : (i + 1);
                if(SUCCEEDED(pMD->GetParamForMethodIndex(methodDef, idx, &paramDef)))
                    pMD->GetParamProps(paramDef, NULL, NULL, wParamName, mdNameLen, &paramNameLen, NULL, NULL, NULL, NULL);
            }
            if(_wcslen(wParamName) == 0)
                swprintf_s(wParamName, mdNameLen, W("param_%d\0"), i);

            ToRelease<ICorDebugValue> pValue;
            ULONG cArgsFetched;
            Status = pParamEnum->Next(1, &pValue, &cArgsFetched);

            if (FAILED(Status))
                continue;

            if (Status == S_FALSE)
                break;

            std::string paramName = to_utf8(wParamName/*, paramNameLen*/);

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
            Status = Modules::GetFrameNamedLocalVariable(pModule, pILFrame, methodDef, i, paramName, &pValue, &ilStart, &ilEnd);

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