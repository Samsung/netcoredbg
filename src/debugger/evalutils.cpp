// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/evalutils.h"
#include "utils/utf.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"


namespace netcoredbg
{

namespace EvalUtils
{

    static std::vector<std::string> ParseGenericParams(const std::string &part, std::string &typeName)
    {
        std::vector<std::string> result;

        std::size_t start = part.find('<');
        if (start == std::string::npos)
        {
            typeName = part;
            return result;
        }

        int paramDepth = 0;
        bool inArray = false;

        result.push_back("");

        for (std::size_t i = start; i < part.size(); i++)
        {
            char c = part.at(i);
            switch(c)
            {
                case ',':
                    if (paramDepth == 1 && !inArray)
                    {
                        result.push_back("");
                        continue;
                    }
                    break;
                case '[':
                    inArray = true;
                    break;
                case ']':
                    inArray = false;
                    break;
                case '<':
                    paramDepth++;
                    if (paramDepth == 1) continue;
                    break;
                case '>':
                    paramDepth--;
                    if (paramDepth == 0) continue;
                    break;
                default:
                    break;
            }
            result.back() += c;
        }
        typeName = part.substr(0, start) + '`' + std::to_string(result.size());
        return result;
    }

    static std::vector<std::string> GatherParameters(const std::vector<std::string> &parts, int indexEnd)
    {
        std::vector<std::string> result;
        for (int i = 0; i < indexEnd; i++)
        {
            std::string typeName;
            std::vector<std::string> params = ParseGenericParams(parts[i], typeName);
            result.insert(result.end(), params.begin(), params.end());
        }
        return result;
    }

    static mdTypeDef GetTypeTokenForName(IMetaDataImport *pMD, mdTypeDef tkEnclosingClass, const std::string &name)
    {
        mdTypeDef typeToken = mdTypeDefNil;
        pMD->FindTypeDefByName(reinterpret_cast<LPCWSTR>(to_utf16(name).c_str()), tkEnclosingClass, &typeToken);
        return typeToken;
    }

    static HRESULT FindTypeInModule(ICorDebugModule *pModule, const std::vector<std::string> &parts, int &nextPart, mdTypeDef &typeToken)
    {
        HRESULT Status;

        ToRelease<IUnknown> pMDUnknown;
        ToRelease<IMetaDataImport> pMD;
        IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
        IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

        std::string currentTypeName;

        // Search for type in module
        for (int i = nextPart; i < (int)parts.size(); i++)
        {
            std::string name;
            ParseGenericParams(parts[i], name);
            currentTypeName += (currentTypeName.empty() ? "" : ".") + name;

            typeToken = GetTypeTokenForName(pMD, mdTypeDefNil, currentTypeName);
            if (typeToken != mdTypeDefNil)
            {
                nextPart = i + 1;
                break;
            }
        }

        if (typeToken == mdTypeDefNil) // type not found, continue search in next module
            return E_FAIL;

        // Resolve nested class
        for (int j = nextPart; j < (int)parts.size(); j++)
        {
            std::string name;
            ParseGenericParams(parts[j], name);
            mdTypeDef classToken = GetTypeTokenForName(pMD, typeToken, name);
            if (classToken == mdTypeDefNil)
                break;
            typeToken = classToken;
            nextPart = j + 1;
        }

        return S_OK;
    }

    HRESULT GetType(const std::string &typeName, ICorDebugThread *pThread, Modules *pModules, ICorDebugType **ppType)
    {
        HRESULT Status;
        std::vector<int> ranks;
        std::vector<std::string> classParts = ParseType(typeName, ranks);
        if (classParts.size() == 1)
            classParts[0] = TypePrinter::RenameToSystem(classParts[0]);

        ToRelease<ICorDebugType> pType;
        int nextClassPart = 0;
        IfFailRet(FindType(classParts, nextClassPart, pThread, pModules, nullptr, &pType));

        if (!ranks.empty())
        {
            ToRelease<ICorDebugAppDomain2> pAppDomain2;
            ToRelease<ICorDebugAppDomain> pAppDomain;
            IfFailRet(pThread->GetAppDomain(&pAppDomain));
            IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

            for (auto irank = ranks.rbegin(); irank != ranks.rend(); ++irank)
            {
                ToRelease<ICorDebugType> pElementType(std::move(pType));
                IfFailRet(pAppDomain2->GetArrayOrPointerType(
                    *irank > 1 ? ELEMENT_TYPE_ARRAY : ELEMENT_TYPE_SZARRAY,
                    *irank,
                    pElementType,
                    &pType));        // NOLINT(clang-analyzer-cplusplus.Move)
            }
        }

        *ppType = pType.Detach();
        return S_OK;
    }

    std::vector<std::string> ParseType(const std::string &expression, std::vector<int> &ranks)
    {
        std::vector<std::string> result;
        int paramDepth = 0;

        result.push_back("");

        for (char c : expression)
        {
            switch(c)
            {
                case '.':
                    if (paramDepth == 0)
                    {
                        result.push_back("");
                        continue;
                    }
                    break;
                case '[':
                    if (paramDepth == 0)
                    {
                        ranks.push_back(1);
                        continue;
                    }
                    break;
                case ']':
                    if (paramDepth == 0)
                        continue;
                    break;
                case ',':
                    if (paramDepth == 0)
                    {
                        if (!ranks.empty())
                            ranks.back()++;
                        continue;
                    }
                    break;
                case '<':
                    paramDepth++;
                    break;
                case '>':
                    paramDepth--;
                    break;
                case ' ':
                    continue;
                default:
                    break;
            }
            result.back() += c;
        }
        return result;
    }

    static HRESULT ResolveParameters(
        const std::vector<std::string> &params,
        ICorDebugThread *pThread,
        Modules *pModules,
        std::vector< ToRelease<ICorDebugType> > &types)
    {
        HRESULT Status;
        for (auto &p : params)
        {
            ICorDebugType *tmpType;
            IfFailRet(EvalUtils::GetType(p, pThread, pModules, &tmpType));
            types.emplace_back(tmpType);
        }
        return S_OK;
    }

    HRESULT FindType(const std::vector<std::string> &parts, int &nextPart, ICorDebugThread *pThread, Modules *pModules,
                     ICorDebugModule *pModule, ICorDebugType **ppType, ICorDebugModule **ppModule)
    {
        HRESULT Status;

        if (pModule)
            pModule->AddRef();
        ToRelease<ICorDebugModule> pTypeModule(pModule);

        mdTypeDef typeToken = mdTypeDefNil;

        if (!pTypeModule)
        {
            pModules->ForEachModule([&](ICorDebugModule *pModule)->HRESULT {
                if (typeToken != mdTypeDefNil) // already found
                    return S_OK;

                if (SUCCEEDED(FindTypeInModule(pModule, parts, nextPart, typeToken)))
                {
                    pModule->AddRef();
                    pTypeModule = pModule;
                }
                return S_OK;
            });
        }
        else
        {
            FindTypeInModule(pTypeModule, parts, nextPart, typeToken);
        }

        if (typeToken == mdTypeDefNil)
            return E_FAIL;

        if (ppType)
        {
            std::vector<std::string> params = GatherParameters(parts, nextPart);
            std::vector< ToRelease<ICorDebugType> > types;
            IfFailRet(ResolveParameters(params, pThread, pModules, types));

            ToRelease<ICorDebugClass> pClass;
            IfFailRet(pTypeModule->GetClassFromToken(typeToken, &pClass));

            ToRelease<ICorDebugClass2> pClass2;
            IfFailRet(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID*) &pClass2));

            ToRelease<IUnknown> pMDUnknown;
            IfFailRet(pTypeModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
            ToRelease<IMetaDataImport> pMD;
            IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

            DWORD flags;
            ULONG nameLen;
            mdToken tkExtends;
            IfFailRet(pMD->GetTypeDefProps(typeToken, nullptr, 0, &nameLen, &flags, &tkExtends));

            std::string eTypeName;
            IfFailRet(TypePrinter::NameForToken(tkExtends, pMD, eTypeName, true, nullptr));

            bool isValueType = eTypeName == "System.ValueType" || eTypeName == "System.Enum";
            CorElementType et = isValueType ? ELEMENT_TYPE_VALUETYPE : ELEMENT_TYPE_CLASS;

            ToRelease<ICorDebugType> pType;
            IfFailRet(pClass2->GetParameterizedType(et, static_cast<uint32_t>(types.size()), (ICorDebugType **)types.data(), &pType));

            *ppType = pType.Detach();
        }
        if (ppModule)
            *ppModule = pTypeModule.Detach();

        return S_OK;
    }

} // namespace EvalHelper

} // namespace netcoredbg
