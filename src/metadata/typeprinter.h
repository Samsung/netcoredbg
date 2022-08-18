// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <list>
#include <string>
#include <vector>

namespace netcoredbg
{

namespace TypePrinter
{

    HRESULT AddGenericArgs(ICorDebugFrame *pFrame, std::list<std::string> &args);
    HRESULT NameForTypeDef(mdTypeDef tkTypeDef, IMetaDataImport *pImport, std::string &mdName,
                           std::list<std::string> *args);
    HRESULT NameForToken(mdToken mb, IMetaDataImport *pImport, std::string &mdName, bool bClassName,
                         std::list<std::string> *args);
    HRESULT NameForTypeByToken(mdToken mb, IMetaDataImport *pImport, std::string &mdName, std::list<std::string> *args);
    HRESULT NameForTypeByType(ICorDebugType *pType, std::string &mdName);
    HRESULT NameForTypeByValue(ICorDebugValue *pValue, std::string &mdName);
    void NameForTypeSig(PCCOR_SIGNATURE typePtr, ICorDebugType *enclosingType, IMetaDataImport *pImport, std::string &typeName);
    HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &output);
    HRESULT GetTypeOfValue(ICorDebugValue *pValue, std::string &output);
    HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &elementType, std::string &arrayType);
    HRESULT GetMethodName(ICorDebugFrame *pFrame, std::string &output);
    HRESULT GetTypeAndMethod(ICorDebugFrame *pFrame, std::string &typeName, std::string &methodName);
    std::string RenameToSystem(const std::string &typeName);
    std::string RenameToCSharp(const std::string &typeName);

} // namespace TypePrinter

} // namespace netcoredbg
