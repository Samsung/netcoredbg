// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules.h"

#include <string>
#include <vector>
#include <functional>
#include <algorithm>

#include "metadata/typeprinter.h"
#include "utils/platform.h"
#include "managed/interop.h"

namespace netcoredbg
{

// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggernonusercodeattribute
// This attribute suppresses the display of these adjunct types and members in the debugger window and
// automatically steps through, rather than into, designer provided code.
const char DebuggerAttribute::NonUserCode[] = "System.Diagnostics.DebuggerNonUserCodeAttribute..ctor";
// Check `DebuggerStepThroughAttribute` for method and class.
// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggerstepthroughattribute
// Instructs the debugger to step through the code instead of stepping into the code.
const char DebuggerAttribute::StepThrough[] = "System.Diagnostics.DebuggerStepThroughAttribute..ctor";
// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggerhiddenattribute
// ... debugger does not stop in a method marked with this attribute and does not allow a breakpoint to be set in the method.
// https://docs.microsoft.com/en-us/dotnet/visual-basic/misc/bc40051
// System.Diagnostics.DebuggerHiddenAttribute does not affect 'Get' or 'Set' when applied to the Property definition.
// Apply the attribute directly to the 'Get' and 'Set' procedures as appropriate.
const char DebuggerAttribute::Hidden[] = "System.Diagnostics.DebuggerHiddenAttribute..ctor";

bool ForEachAttribute(IMetaDataImport *pMD, mdToken tok, std::function<HRESULT(const std::string &AttrName)> cb)
{
    bool found = false;
    ULONG numAttributes = 0;
    HCORENUM fEnum = NULL;
    mdCustomAttribute attr;
    while(SUCCEEDED(pMD->EnumCustomAttributes(&fEnum, tok, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
    {
        std::string mdName;
        mdToken ptkObj = mdTokenNil;
        mdToken ptkType = mdTokenNil;
        if (FAILED(pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, nullptr, nullptr)) ||
            FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, nullptr)))
            continue;

        found = cb(mdName);
        if (found)
            break;
    }
    pMD->CloseEnum(fEnum);
    return found;
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName)
{
    return ForEachAttribute(pMD, tok, [&attrName](const std::string &AttrName) -> bool
    {
        return AttrName == attrName;
    });
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames)
{
    return ForEachAttribute(pMD, tok, [&attrNames](const std::string &AttrName) -> bool
    {
        return std::find(attrNames.begin(), attrNames.end(), AttrName) != attrNames.end();
    });
}

static HRESULT GetNonJMCMethodsForTypeDef(
    IMetaDataImport *pMD,
    PVOID pSymbolReaderHandle,
    mdTypeDef typeDef,
    std::vector<mdToken> &excludeMethods)
{
    static std::vector<std::string> attrNames{DebuggerAttribute::NonUserCode, DebuggerAttribute::StepThrough, DebuggerAttribute::Hidden};

    ULONG numMethods = 0;
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef;
    while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {
        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};

        if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef,
                                       szFunctionName, _countof(szFunctionName), &nameLen,
                                       nullptr, nullptr, nullptr, nullptr, nullptr)))
            continue;

        if (HasAttribute(pMD, methodDef, attrNames))
            excludeMethods.push_back(methodDef);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

static HRESULT GetNonJMCClassesAndMethods(ICorDebugModule *pModule, PVOID pSymbolReaderHandle, std::vector<mdToken> &excludeTokens)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    static std::vector<std::string> attrNames{DebuggerAttribute::NonUserCode, DebuggerAttribute::StepThrough};

    ULONG numTypedefs = 0;
    HCORENUM fEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMD->EnumTypeDefs(&fEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        if (HasAttribute(pMD, typeDef, attrNames))
            excludeTokens.push_back(typeDef);
        else
            GetNonJMCMethodsForTypeDef(pMD, pSymbolReaderHandle, typeDef, excludeTokens);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

HRESULT DisableJMCByAttributes(ICorDebugModule *pModule, PVOID pSymbolReaderHandle)
{
    std::vector<mdToken> excludeTokens;

    GetNonJMCClassesAndMethods(pModule, pSymbolReaderHandle, excludeTokens);

    for (mdToken token : excludeTokens)
    {
        if (TypeFromToken(token) == mdtMethodDef)
        {
            ToRelease<ICorDebugFunction> pFunction;
            ToRelease<ICorDebugFunction2> pFunction2;
            if (FAILED(pModule->GetFunctionFromToken(token, &pFunction)) ||
                FAILED(pFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID *)&pFunction2)))
                continue;

            pFunction2->SetJMCStatus(FALSE);
        }
        else if (TypeFromToken(token) == mdtTypeDef)
        {
            ToRelease<ICorDebugClass> pClass;
            ToRelease<ICorDebugClass2> pClass2;
            if (FAILED(pModule->GetClassFromToken(token, &pClass)) ||
                FAILED(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID *)&pClass2)))
                continue;

            pClass2->SetJMCStatus(FALSE);
        }
    }

    return S_OK;
}

} // namespace netcoredbg
