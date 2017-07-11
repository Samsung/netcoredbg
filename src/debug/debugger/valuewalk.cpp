#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <iomanip>

#include "torelease.h"
#include "arrayholder.h"
#include "cputil.h"

// Modules
HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd);

#include "typeprinter.h"

// Valueprint
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = NULL);

typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

extern std::mutex g_currentThreadMutex;
extern ICorDebugThread *g_currentThread;

std::mutex g_evalMutex;
std::condition_variable g_evalCV;
bool g_evalComplete = false;

void NotifyEvalComplete()
{
    std::lock_guard<std::mutex> lock(g_evalMutex);
    g_evalComplete = true;
    g_evalMutex.unlock();
    g_evalCV.notify_one();
}

HRESULT EvalProperty(
    mdMethodDef methodDef,
    ICorDebugModule *pModule,
    ICorDebugType *pType,
    ICorDebugValue *pInputValue,
    bool is_static,
    ICorDebugValue **ppEvalResult)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugEval> pEval;

    ToRelease<ICorDebugProcess> pProcess;
    {
        // g_currentThreadMutex should be locked by caller function
        //std::lock_guard<std::mutex> lock(g_currentThreadMutex);

        IfFailRet(g_currentThread->GetProcess(&pProcess));
        IfFailRet(g_currentThread->CreateEval(&pEval));
    }

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(methodDef, &pFunc));

    ToRelease<ICorDebugTypeEnum> pTypeEnum;

    std::vector< ToRelease<ICorDebugType> > typeParams;
    if(SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ICorDebugType *curType;
        ULONG fetched = 0;
        while(SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
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
        is_static ? 0 : 1,
        is_static ? NULL : &pInputValue
    ));

    std::unique_lock<std::mutex> lock(g_evalMutex);
    g_evalComplete = false;
    IfFailRet(pProcess->Continue(0));
    g_evalCV.wait(lock, []{return g_evalComplete;});

    return pEval->GetResult(ppEvalResult);
}

static HRESULT WalkMembers(ICorDebugValue *pInputValue, ICorDebugILFrame *pILFrame, ICorDebugType *pTypeCast, WalkMembersCallback cb)
{
    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull) return S_OK;

    ToRelease<ICorDebugArrayValue> pArrayValue;
    if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayValue)))
    {
        ULONG32 nRank;
        IfFailRet(pArrayValue->GetRank(&nRank));

        ULONG32 cElements;
        IfFailRet(pArrayValue->GetCount(&cElements));

        for (ULONG32 i = 0; i < cElements; ++i)
        {
            ToRelease<ICorDebugValue> pElementValue;
            pArrayValue->GetElementAtPosition(i, &pElementValue);
            IfFailRet(cb(mdMethodDefNil, nullptr, nullptr, pElementValue, false, "[" + std::to_string(i) + "]"));
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

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if(baseTypeName != "System.Object"  && baseTypeName != "System.ValueType")
        {
            // Add fields of base class
            IfFailRet(WalkMembers(pInputValue, pILFrame, pBaseType, cb));
        }
    }

    std::string className;
    TypePrinter::GetTypeOfValue(pType, className);

    std::unordered_set<std::string> backedProperies;

    ULONG numFields = 0;
    HCORENUM fEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        if(SUCCEEDED(pMD->GetFieldProps(fieldDef, NULL, mdName, mdNameLen, &nameLen, &fieldAttr, NULL, NULL, NULL, NULL, NULL)))
        {
            std::string name = to_utf8(mdName, nameLen);

            if(fieldAttr & fdLiteral)
                continue;

            bool is_static = (fieldAttr & fdStatic);

            ToRelease<ICorDebugValue> pFieldVal;

            if (fieldAttr & fdStatic)
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

            if(pFieldVal != NULL)
            {
                if (name[0] == '<')
                {
                    size_t endOffset = name.rfind('>');
                    name = name.substr(1, endOffset - 1);
                    backedProperies.insert(name);
                }

                IfFailRet(cb(mdMethodDefNil, pModule, pType, pFieldVal, is_static, name));
            }
            else
            {
                // no need for backing field when we can not get its value
                if (name[0] == '<')
                    continue;

                IfFailRet(cb(mdMethodDefNil, pModule, pType, nullptr, is_static, name));
            }
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

            std::string name = to_utf8(propertyName, propertyNameLen);

            if (backedProperies.find(name) != backedProperies.end())
                continue;

            bool is_static = (getterAttr & mdStatic);

            IfFailRet(cb(mdGetter, pModule, pType, nullptr, is_static, name));
        }
    }
    pMD->CloseEnum(propEnum);

    return S_OK;
}

HRESULT WalkMembers(ICorDebugValue *pValue, ICorDebugILFrame *pILFrame, WalkMembersCallback cb)
{
    return WalkMembers(pValue, pILFrame, nullptr, cb);
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

    if (cParams > 0)
    {
        DWORD methodAttr = 0;
        IfFailRet(pMD->GetMethodProps(methodDef, NULL, NULL, 0, NULL, &methodAttr, NULL, NULL, NULL, NULL));

        for (ULONG i = 0; i < cParams; i++)
        {
            ULONG paramNameLen = 0;
            mdParamDef paramDef;
            WCHAR paramName[mdNameLen] = W("\0");

            if(i == 0 && (methodAttr & mdStatic) == 0)
                swprintf_s(paramName, mdNameLen, W("this\0"));
            else
            {
                int idx = ((methodAttr & mdStatic) == 0)? i : (i + 1);
                if(SUCCEEDED(pMD->GetParamForMethodIndex(methodDef, idx, &paramDef)))
                    pMD->GetParamProps(paramDef, NULL, NULL, paramName, mdNameLen, &paramNameLen, NULL, NULL, NULL, NULL);
            }
            if(_wcslen(paramName) == 0)
                swprintf_s(paramName, mdNameLen, W("param_%d\0"), i);

            ToRelease<ICorDebugValue> pValue;
            ULONG cArgsFetched;
            Status = pParamEnum->Next(1, &pValue, &cArgsFetched);

            if (FAILED(Status))
                continue;

            if (Status == S_FALSE)
                break;

            IfFailRet(cb(pILFrame, pValue, to_utf8(paramName, paramNameLen)));
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
            Status = GetFrameNamedLocalVariable(pModule, pILFrame, methodDef, i, paramName, &pValue, &ilStart, &ilEnd);

            if (FAILED(Status))
                continue;

            if (currentIlOffset < ilStart || currentIlOffset >= ilEnd)
                continue;

            if (Status == S_FALSE)
                break;

            IfFailRet(cb(pILFrame, pValue, paramName));
        }
    }

    return S_OK;
}