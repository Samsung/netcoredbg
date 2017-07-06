#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

#include "torelease.h"

// Modules
HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue);


HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output)
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

    std::stringstream ss;

    ss << "variables=[";

    const char *sep = "";

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

            char cParamName[mdNameLen] = {0};
            WideCharToMultiByte(CP_UTF8, 0, paramName, (int)(_wcslen(paramName) + 1), cParamName, _countof(cParamName), NULL, NULL);

            ss << sep << "{name=\"" << cParamName << "\"}";
            sep = ",";
        }
    }

    ULONG cLocals = 0;
    ToRelease<ICorDebugValueEnum> pLocalsEnum;

    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));
    IfFailRet(pLocalsEnum->GetCount(&cLocals));
    if (cLocals > 0)
    {
        for (ULONG i=0; i < cLocals; i++)
        {
            std::string paramName;

            ToRelease<ICorDebugValue> pValue;
            Status = GetFrameNamedLocalVariable(pModule, pILFrame, methodDef, i, paramName, &pValue);

            if (FAILED(Status))
                continue;

            if (Status == S_FALSE)
                break;

            ss << sep << "{name=\"" << paramName << "\"}";
            sep = ",";
        }
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}
