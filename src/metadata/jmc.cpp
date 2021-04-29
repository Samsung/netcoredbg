// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules.h"

#include <string>
#include <vector>
#include <list>
#include <unordered_set>

#include "metadata/typeprinter.h"
#include "platform.h"
#include "managed/interop.h"
#include "utils/utf.h"

namespace netcoredbg
{

static const char *g_nonUserCode = "System.Diagnostics.DebuggerNonUserCodeAttribute..ctor";
static const char *g_stepThrough = "System.Diagnostics.DebuggerStepThroughAttribute..ctor";
// TODO: DebuggerStepThroughAttribute also affects breakpoints when JMC is enabled

// From ECMA-335
static const std::unordered_set<WSTRING> g_operatorMethodNames
{
// Unary operators
    W("op_Decrement"),                    // --
    W("op_Increment"),                    // ++
    W("op_UnaryNegation"),                // - (unary)
    W("op_UnaryPlus"),                    // + (unary)
    W("op_LogicalNot"),                   // !
    W("op_True"),                         // Not defined
    W("op_False"),                        // Not defined
    W("op_AddressOf"),                    // & (unary)
    W("op_OnesComplement"),               // ~
    W("op_PointerDereference"),           // * (unary)
// Binary operators
    W("op_Addition"),                     // + (binary)
    W("op_Subtraction"),                  // - (binary)
    W("op_Multiply"),                     // * (binary)
    W("op_Division"),                     // /
    W("op_Modulus"),                      // %
    W("op_ExclusiveOr"),                  // ^
    W("op_BitwiseAnd"),                   // & (binary)
    W("op_BitwiseOr"),                    // |
    W("op_LogicalAnd"),                   // &&
    W("op_LogicalOr"),                    // ||
    W("op_Assign"),                       // Not defined (= is not the same)
    W("op_LeftShift"),                    // <<
    W("op_RightShift"),                   // >>
    W("op_SignedRightShift"),             // Not defined
    W("op_UnsignedRightShift"),           // Not defined
    W("op_Equality"),                     // ==
    W("op_GreaterThan"),                  // >
    W("op_LessThan"),                     // <
    W("op_Inequality"),                   // !=
    W("op_GreaterThanOrEqual"),           // >=
    W("op_LessThanOrEqual"),              // <=
    W("op_UnsignedRightShiftAssignment"), // Not defined
    W("op_MemberSelection"),              // ->
    W("op_RightShiftAssignment"),         // >>=
    W("op_MultiplicationAssignment"),     // *=
    W("op_PointerToMemberSelection"),     // ->*
    W("op_SubtractionAssignment"),        // -=
    W("op_ExclusiveOrAssignment"),        // ^=
    W("op_LeftShiftAssignment"),          // <<=
    W("op_ModulusAssignment"),            // %=
    W("op_AdditionAssignment"),           // +=
    W("op_BitwiseAndAssignment"),         // &=
    W("op_BitwiseOrAssignment"),          // |=
    W("op_Comma"),                        // ,
    W("op_DivisionAssignment")            // /=
};

static bool HasAttributes(IMetaDataImport *pMD, mdToken tok, bool checkNonUserCode = true, bool checkStepThrough = true)
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

        if ((checkNonUserCode && mdName == g_nonUserCode) || 
            (checkStepThrough && mdName == g_stepThrough))
        {
            found = true;
            break;
        }
    }
    pMD->CloseEnum(fEnum);

    return found;
}

static bool HasNonUserCodeAttribute(IMetaDataImport *pMD, mdToken tok)
{
    return HasAttributes(pMD, tok, true, false);
}

static HRESULT GetNonJMCMethodsForTypeDef(
    IMetaDataImport *pMD,
    PVOID pSymbolReaderHandle,
    mdTypeDef typeDef,
    std::vector<mdToken> &excludeMethods)
{
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

        if (g_operatorMethodNames.find(szFunctionName) != g_operatorMethodNames.end()
            || HasAttributes(pMD, methodDef)
            || !Interop::HasSourceLocation(pSymbolReaderHandle, methodDef))
        {
            excludeMethods.push_back(methodDef);
        }
    }
    pMD->CloseEnum(fEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdMethodDef mdSetter;
        mdMethodDef mdGetter;
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
                                            nullptr, nullptr, nullptr, &mdSetter, &mdGetter, nullptr, 0, nullptr)))
        {
            if (mdSetter != mdMethodDefNil)
                excludeMethods.push_back(mdSetter);
            if (mdGetter != mdMethodDefNil)
                excludeMethods.push_back(mdGetter);
        }
    }
    pMD->CloseEnum(propEnum);

    return S_OK;
}

static HRESULT GetNonJMCClassesAndMethods(ICorDebugModule *pModule, PVOID pSymbolReaderHandle, std::vector<mdToken> &excludeTokens)
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
        if (HasNonUserCodeAttribute(pMD, typeDef))
            excludeTokens.push_back(typeDef);
        else
            GetNonJMCMethodsForTypeDef(pMD, pSymbolReaderHandle, typeDef, excludeTokens);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

// Disable JMC by exception list: operators, getters/setters and by attributes (DebuggerNonUserCode and DebuggerStepThrough).
HRESULT Modules::DisableJMCByExceptionList(ICorDebugModule *pModule, PVOID pSymbolReaderHandle)
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
