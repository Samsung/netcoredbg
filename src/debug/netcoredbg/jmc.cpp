#include "common.h"

#include <string>
#include <vector>
#include <list>

#include "typeprinter.h"

std::string GetFileName(const std::string &path);


static const char *g_nonUserCode = "System.Diagnostics.DebuggerNonUserCodeAttribute..ctor";

bool ShouldLoadSymbolsForModule(const std::string &moduleName)
{
    std::string name = GetFileName(moduleName);
    if (name.find("System.") == 0 || name.find("SOS.") == 0)
        return false;
    return true;
}

static bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const std::string &attrName)
{
    bool found = false;

    ULONG numAttributes = 0;
    HCORENUM fEnum = NULL;
    mdCustomAttribute attr;
    while(SUCCEEDED(pMD->EnumCustomAttributes(&fEnum, tok, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
    {
        mdToken ptkObj = mdTokenNil;
        mdToken ptkType = mdTokenNil;
        pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, nullptr, nullptr);

        std::string mdName;
        std::list<std::string> emptyArgs;
        TypePrinter::NameForToken(ptkType, pMD, mdName, true, emptyArgs);

        if (mdName == attrName)
        {
            found = true;
            break;
        }
    }
    pMD->CloseEnum(fEnum);

    return found;
}

static HRESULT GetNonJMCMethodsForTypeDef(IMetaDataImport *pMD, mdTypeDef typeDef, std::vector<mdToken> &excludeMethods)
{
    HRESULT Status;

    ULONG numMethods = 0;
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef;
    while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {
        HRESULT hr;
        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};

        Status = pMD->GetMethodProps(methodDef, &memTypeDef,
                                     szFunctionName, _countof(szFunctionName), &nameLen,
                                     nullptr, nullptr, nullptr, nullptr, nullptr);

        if (HasAttribute(pMD, methodDef, g_nonUserCode))
            excludeMethods.push_back(methodDef);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

static HRESULT GetNonJMCClassesAndMethods(ICorDebugModule *pModule, std::vector<mdToken> &excludeTokens)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numTypedefs = 0;
    HCORENUM fEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMD->EnumTypeDefs(&fEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        if (HasAttribute(pMD, typeDef, g_nonUserCode))
            excludeTokens.push_back(typeDef);
        GetNonJMCMethodsForTypeDef(pMD, typeDef, excludeTokens);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

HRESULT SetJMCFromAttributes(ICorDebugModule *pModule)
{
    std::vector<mdToken> excludeTokens;

    GetNonJMCClassesAndMethods(pModule, excludeTokens);

    for (mdToken token : excludeTokens)
    {
        if (TypeFromToken(token) == mdtMethodDef)
        {
            ToRelease<ICorDebugFunction> pFunction;
            ToRelease<ICorDebugFunction2> pFunction2;
            if (FAILED(pModule->GetFunctionFromToken(token, &pFunction)))
                continue;
            if (FAILED(pFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID *)&pFunction2)))
                continue;

            pFunction2->SetJMCStatus(FALSE);
        }
        else if (TypeFromToken(token) == mdtTypeDef)
        {
            ToRelease<ICorDebugClass> pClass;
            ToRelease<ICorDebugClass2> pClass2;
            if (FAILED(pModule->GetClassFromToken(token, &pClass)))
                continue;
            if (FAILED(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID *)&pClass2)))
                continue;

            pClass2->SetJMCStatus(FALSE);
        }
    }

    return S_OK;
}
