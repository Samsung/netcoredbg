// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "modules.h"

#include <string>
#include <vector>
#include <list>
#include <unordered_set>

#include "typeprinter.h"
#include "platform.h"
#include "symbolreader.h"
#include "cputil.h"


static const char *g_nonUserCode = "System.Diagnostics.DebuggerNonUserCodeAttribute..ctor";
static const char *g_stepThrough = "System.Diagnostics.DebuggerStepThroughAttribute..ctor";
// TODO: DebuggerStepThroughAttribute also affects breakpoints when JMC is enabled

// From ECMA-335
static const std::unordered_set<std::string> g_operatorMethodNames
{
// Unary operators
    "op_Decrement",                    // --
    "op_Increment",                    // ++
    "op_UnaryNegation",                // - (unary)
    "op_UnaryPlus",                    // + (unary)
    "op_LogicalNot",                   // !
    "op_True",                         // Not defined
    "op_False",                        // Not defined
    "op_AddressOf",                    // & (unary)
    "op_OnesComplement",               // ~
    "op_PointerDereference",           // * (unary)
// Binary operators
    "op_Addition",                     // + (binary)
    "op_Subtraction",                  // - (binary)
    "op_Multiply",                     // * (binary)
    "op_Division",                     // /
    "op_Modulus",                      // %
    "op_ExclusiveOr",                  // ^
    "op_BitwiseAnd",                   // & (binary)
    "op_BitwiseOr",                    // |
    "op_LogicalAnd",                   // &&
    "op_LogicalOr",                    // ||
    "op_Assign",                       // Not defined (= is not the same)
    "op_LeftShift",                    // <<
    "op_RightShift",                   // >>
    "op_SignedRightShift",             // Not defined
    "op_UnsignedRightShift",           // Not defined
    "op_Equality",                     // ==
    "op_GreaterThan",                  // >
    "op_LessThan",                     // <
    "op_Inequality",                   // !=
    "op_GreaterThanOrEqual",           // >=
    "op_LessThanOrEqual",              // <=
    "op_UnsignedRightShiftAssignment", // Not defined
    "op_MemberSelection",              // ->
    "op_RightShiftAssignment",         // >>=
    "op_MultiplicationAssignment",     // *=
    "op_PointerToMemberSelection",     // ->*
    "op_SubtractionAssignment",        // -=
    "op_ExclusiveOrAssignment",        // ^=
    "op_LeftShiftAssignment",          // <<=
    "op_ModulusAssignment",            // %=
    "op_AdditionAssignment",           // +=
    "op_BitwiseAndAssignment",         // &=
    "op_BitwiseOrAssignment",          // |=
    "op_Comma",                        // ,
    "op_DivisionAssignment"            // /=
};

bool Modules::ShouldLoadSymbolsForModule(const std::string &moduleName)
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

static bool HasSourceLocation(SymbolReader *symbolReader, mdMethodDef methodDef)
{
    std::vector<SymbolReader::SequencePoint> points;
    if (FAILED(symbolReader->GetSequencePoints(methodDef, points)))
        return false;

    for (auto &p : points)
    {
        if (p.startLine != 0 && p.startLine != SymbolReader::HiddenLine)
            return true;
    }
    return false;
}

static HRESULT GetNonJMCMethodsForTypeDef(
    IMetaDataImport *pMD,
    SymbolReader *sr,
    mdTypeDef typeDef,
    std::vector<mdToken> &excludeMethods)
{
    HRESULT Status;

    ULONG numMethods = 0;
    HCORENUM fEnum = NULL;
    mdMethodDef methodDef;
    while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {
        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};

        Status = pMD->GetMethodProps(methodDef, &memTypeDef,
                                     szFunctionName, _countof(szFunctionName), &nameLen,
                                     nullptr, nullptr, nullptr, nullptr, nullptr);

        if (FAILED(Status))
            continue;

        if ((g_operatorMethodNames.find(to_utf8(szFunctionName)) != g_operatorMethodNames.end())
            || HasAttribute(pMD, methodDef, g_nonUserCode)
            || HasAttribute(pMD, methodDef, g_stepThrough)
            || !HasSourceLocation(sr, methodDef))
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
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef,
                                            nullptr,
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            &mdSetter,
                                            &mdGetter,
                                            nullptr,
                                            0,
                                            nullptr)))
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

static HRESULT GetNonJMCClassesAndMethods(ICorDebugModule *pModule, SymbolReader *sr, std::vector<mdToken> &excludeTokens)
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
        else
            GetNonJMCMethodsForTypeDef(pMD, sr, typeDef, excludeTokens);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

HRESULT Modules::SetJMCFromAttributes(ICorDebugModule *pModule, SymbolReader *symbolReader)
{
    std::vector<mdToken> excludeTokens;

    GetNonJMCClassesAndMethods(pModule, symbolReader, excludeTokens);

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
