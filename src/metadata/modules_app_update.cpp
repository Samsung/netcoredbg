// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules_app_update.h"

#include <string>
#include "metadata/typeprinter.h"
#include "utils/utf.h"

namespace netcoredbg
{

// Get data from 'MetadataUpdateHandler' attribute.
// https://docs.microsoft.com/en-us/dotnet/api/system.reflection.metadata.metadataupdatehandlerattribute?view=net-6.0
// Note, provided by attribute type have same format as Type.GetType(String) argument
// https://docs.microsoft.com/en-us/dotnet/api/system.type.gettype?view=net-6.0#system-type-gettype
static HRESULT GetUpdateHandlerTypesForModule(IMetaDataImport *pMD, std::vector<std::string> &updateHandlerTypes)
{
    static const std::string metadataUpdateHandlerAttribute = "System.Reflection.Metadata.MetadataUpdateHandlerAttribute..ctor";

    ULONG numAttributes = 0;
    HCORENUM fEnum = NULL;
    mdCustomAttribute attr;
    while(SUCCEEDED(pMD->EnumCustomAttributes(&fEnum, 0, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
    {
        std::string mdName;
        mdToken tkObj = mdTokenNil;
        mdToken tkType = mdTokenNil;
        void const *pBlob = 0;
        ULONG cbSize = 0;
        if (FAILED(pMD->GetCustomAttributeProps(attr, &tkObj, &tkType, &pBlob, &cbSize)) ||
            FAILED(TypePrinter::NameForToken(tkType, pMD, mdName, true, nullptr)))
            continue;

        if (mdName != metadataUpdateHandlerAttribute)
            continue;

        const unsigned char *pCBlob = (const unsigned char *)pBlob;
        ULONG elementSize;

        // 2 bytes - blob prolog 0x0001
        pCBlob += 2;

        ULONG stringSize;
        elementSize = CorSigUncompressData(pCBlob, &stringSize);
        pCBlob += elementSize;
        updateHandlerTypes.emplace_back(pCBlob, pCBlob + stringSize);
    }
    pMD->CloseEnum(fEnum);

    return S_OK;
}

// Parse type name. More info:
// https://docs.microsoft.com/en-us/dotnet/framework/reflection-and-codedom/specifying-fully-qualified-type-names
// Note, in MetadataUpdateHandler attribute case, type name will not have assembly relative parts.
// Backtick (`)      Precedes one or more digits representing the number of type parameters, located at the end of the name of a generic type.
// Brackets ([])     Enclose a generic type argument list, for a constructed generic type; within a type argument list, enclose an assembly-qualified type.
// Comma (,)         Precedes the Assembly name.
// Period (.)        Denotes namespace identifiers.
// Plus sign (+)     Precedes a nested class.
static void ParceTypeName(const std::string &fullName, std::string &mainTypeName, std::vector<std::string> &nestedClasses)
{
    std::string::size_type genericDelimiterPos = fullName.find("`");
    std::string fullTypeName;
    std::string genericTypes;
    if (genericDelimiterPos == std::string::npos)
    {
        fullTypeName = fullName;
    }
    else
    {
        fullTypeName = fullName.substr(0, genericDelimiterPos);
        genericTypes = fullName.substr(genericDelimiterPos);
    }

    // TODO implement generic support (genericTypes) for update handler types

    std::string::size_type nestedClassDelimiterPos = fullTypeName.find("+");
    if (nestedClassDelimiterPos == std::string::npos)
    {
        mainTypeName = fullTypeName;
    }
    else
    {
        mainTypeName = fullTypeName.substr(0, nestedClassDelimiterPos);
        while (nestedClassDelimiterPos != std::string::npos)
        {
            std::string::size_type startPos = nestedClassDelimiterPos + 1;
            nestedClassDelimiterPos = fullTypeName.find("+", startPos);
            if (nestedClassDelimiterPos == std::string::npos)
                nestedClasses.emplace_back(fullName.substr(startPos, fullTypeName.size() - startPos));
            else
                nestedClasses.emplace_back(fullName.substr(startPos, nestedClassDelimiterPos - startPos));
        }
    }
}

// Scan module for 'MetadataUpdateHandler' attributes and store related to UpdateHandlerTypes ICorDebugType objects.
// Note, 'MetadataUpdateHandler' attributes can't be changed/removed/added at Hot Reload.
HRESULT ModulesAppUpdate::AddUpdateHandlerTypesForModule(ICorDebugModule *pModule, IMetaDataImport *pMD)
{
    HRESULT Status;
    std::vector<std::string> updateHandlerTypeNames;
    IfFailRet(GetUpdateHandlerTypesForModule(pMD, updateHandlerTypeNames));

    for (const auto &entry : updateHandlerTypeNames)
    {
        std::string mainTypeName;
        std::vector<std::string> nestedClasses;
        ParceTypeName(entry, mainTypeName, nestedClasses);

        // Resolve main type part.
        mdTypeDef typeToken = mdTypeDefNil;
        IfFailRet(pMD->FindTypeDefByName(reinterpret_cast<LPCWSTR>(to_utf16(mainTypeName).c_str()), mdTypeDefNil, &typeToken));
        if (typeToken == mdTypeDefNil)
            return E_FAIL;

        // Resolve nested type part.
        for (const auto &nestedClassName : nestedClasses)
        {
            mdTypeDef classToken = mdTypeDefNil;
            IfFailRet(pMD->FindTypeDefByName(reinterpret_cast<LPCWSTR>(to_utf16(nestedClassName).c_str()), typeToken, &classToken));
            if (classToken == mdTypeDefNil)
                return E_FAIL;
            typeToken = classToken;
        }

        ToRelease<ICorDebugClass> pClass;
        IfFailRet(pModule->GetClassFromToken(typeToken, &pClass));
        ToRelease<ICorDebugClass2> pClass2;
        IfFailRet(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID*) &pClass2));
        m_modulesUpdateHandlerTypes.emplace_back();
        IfFailRet(pClass2->GetParameterizedType(ELEMENT_TYPE_CLASS, 0, nullptr, &(m_modulesUpdateHandlerTypes.back())));
    }

    return S_OK;
}

void ModulesAppUpdate::CopyModulesUpdateHandlerTypes(std::vector<ToRelease<ICorDebugType>> &modulesUpdateHandlerTypes)
{
    modulesUpdateHandlerTypes.reserve(m_modulesUpdateHandlerTypes.size());
    for (ToRelease<ICorDebugType> &updateHandlerType : m_modulesUpdateHandlerTypes)
    {
        updateHandlerType->AddRef();
        modulesUpdateHandlerTypes.emplace_back(updateHandlerType.GetPtr());
    }
}

} // namespace netcoredbg
