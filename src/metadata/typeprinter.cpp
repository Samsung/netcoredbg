// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/typeprinter.h"

#include <sstream>
#include <unordered_map>
#include <memory>

#include "utils/string_view.h"
#include "utils/torelease.h"
#include "utils/utf.h"


namespace netcoredbg 
{

namespace TypePrinter
{

static std::string ConsumeGenericArgs(const std::string &name, std::list<std::string> &args)
{
    if (args.empty())
        return name;

    std::size_t offset = name.find_last_not_of("0123456789");

    if (offset == std::string::npos || offset == name.size() - 1 || name.at(offset) != '`')
        return name;

    unsigned long numArgs = 0;
    try {
        numArgs = std::stoul(name.substr(offset + 1));
    }
    catch(std::invalid_argument e)
    {
        return name;
    }
    catch (std::out_of_range  e)
    {
        return name;
    }

    if (numArgs == 0 || numArgs > args.size())
    {
        return name;
    }

    std::ostringstream ss;
    ss << name.substr(0, offset);
    ss << "<";
    const char *sep = "";
    while (numArgs--)
    {
        ss << sep;
        sep = ", ";
        ss << args.front();
        args.pop_front();
    }
    ss << ">";
    return ss.str();
}

std::string RenameToSystem(const std::string &typeName)
{
    static const std::unordered_map<std::string, std::string> cs2system = {
        {"void",    "System.Void"},
        {"bool",    "System.Boolean"},
        {"byte",    "System.Byte"},
        {"sbyte",   "System.SByte"},
        {"char",    "System.Char"},
        {"decimal", "System.Decimal"},
        {"double",  "System.Double"},
        {"float",   "System.Single"},
        {"int",     "System.Int32"},
        {"uint",    "System.UInt32"},
        {"long",    "System.Int64"},
        {"ulong",   "System.UInt64"},
        {"object",  "System.Object"},
        {"short",   "System.Int16"},
        {"ushort",  "System.UInt16"},
        {"string",  "System.String"},
        {"IntPtr",  "System.IntPtr"},
        {"UIntPtr", "System.UIntPtr"}
    };
    auto renamed = cs2system.find(typeName);
    return renamed != cs2system.end() ? renamed->second : typeName;
}

std::string RenameToCSharp(const std::string &typeName)
{
    static const std::unordered_map<std::string, std::string> system2cs = {
        {"System.Void",    "void"},
        {"System.Boolean", "bool"},
        {"System.Byte",    "byte"},
        {"System.SByte",   "sbyte"},
        {"System.Char",    "char"},
        {"System.Decimal", "decimal"},
        {"System.Double",  "double"},
        {"System.Single",  "float"},
        {"System.Int32",   "int"},
        {"System.UInt32",  "uint"},
        {"System.Int64",   "long"},
        {"System.UInt64",  "ulong"},
        {"System.Object",  "object"},
        {"System.Int16",   "short"},
        {"System.UInt16",  "ushort"},
        {"System.String",  "string"},
        {"System.IntPtr",  "IntPtr"},
        {"System.UIntPtr", "UIntPtr"}
    };
    auto renamed = system2cs.find(typeName);
    return renamed != system2cs.end() ? renamed->second : typeName;
}

// From metadata.cpp

/**********************************************************************\
* Routine Description:                                                 *
*                                                                      *
*    This function is called to find the name of a TypeDef using       *
*    metadata API.                                                     *
*                                                                      *
\**********************************************************************/
// Caller should guard against exception
HRESULT NameForTypeDef(
    mdTypeDef tkTypeDef,
    IMetaDataImport *pImport,
    std::string &mdName,
    std::list<std::string> *args)
{
    HRESULT Status;
    DWORD flags;
    WCHAR name[mdNameLen];
    ULONG nameLen;

    IfFailRet(pImport->GetTypeDefProps(tkTypeDef, name, _countof(name), &nameLen, &flags, NULL));
    mdName = to_utf8(name/*, nameLen*/);

    if (!IsTdNested(flags))
    {
        if (args)
            mdName = ConsumeGenericArgs(mdName, *args);

        return S_OK;
    }

    mdTypeDef tkEnclosingClass;
    IfFailRet(pImport->GetNestedClassProps(tkTypeDef, &tkEnclosingClass));

    std::string enclosingName;
    IfFailRet(NameForTypeDef(tkEnclosingClass, pImport, enclosingName, args));

    mdName = enclosingName + "." + (args ? ConsumeGenericArgs(mdName, *args) : mdName);

    return S_OK;
}

static HRESULT NameForTypeRef(mdTypeRef tkTypeRef, IMetaDataImport *pImport, std::string &mdName)
{
    // Note, instead of GetTypeDefProps(), GetTypeRefProps() return fully-qualified name.
    // CoreCLR use dynamic allocated or size fixed buffers up to 16kb for GetTypeRefProps().
    HRESULT Status;
    ULONG refNameSize;
    IfFailRet(pImport->GetTypeRefProps(tkTypeRef, NULL, NULL, 0, &refNameSize));

    std::unique_ptr<WCHAR[]> refName(new WCHAR[refNameSize + 1]);
    IfFailRet(pImport->GetTypeRefProps(tkTypeRef, NULL, refName.get(), refNameSize, NULL));

    mdName = to_utf8(refName.get());

    return S_OK;
}

HRESULT NameForTypeByToken(mdToken mb,
                                        IMetaDataImport *pImport,
                                        std::string &mdName,
                                        std::list<std::string> *args)
{
    mdName[0] = L'\0';
    if (TypeFromToken(mb) != mdtTypeDef
        && TypeFromToken(mb) != mdtTypeRef)
    {
        return E_FAIL;
    }

    HRESULT hr;
    if (TypeFromToken(mb) == mdtTypeDef)
    {
        hr = NameForTypeDef(mb, pImport, mdName, args);
    }
    else if (TypeFromToken(mb) == mdtTypeRef)
    {
        hr = NameForTypeRef(mb, pImport, mdName);
    }
    else
    {
        hr = E_FAIL;
    }

    return hr;
}

static HRESULT AddGenericArgs(ICorDebugType *pType, std::list<std::string> &args)
{
    ToRelease<ICorDebugTypeEnum> pTypeEnum;

    if (SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ULONG fetched = 0;
        ToRelease<ICorDebugType> pCurrentTypeParam;

        while (SUCCEEDED(pTypeEnum->Next(1, &pCurrentTypeParam, &fetched)) && fetched == 1)
        {
            std::string name;
            GetTypeOfValue(pCurrentTypeParam, name);
            args.emplace_back(name);
            pCurrentTypeParam.Free();
        }
    }

    return S_OK;
}

HRESULT AddGenericArgs(ICorDebugFrame *pFrame, std::list<std::string> &args)
{
    HRESULT Status;

    ToRelease<ICorDebugILFrame2> pILFrame2;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame2, (LPVOID*) &pILFrame2));

    ToRelease<ICorDebugTypeEnum> pTypeEnum;

    if (SUCCEEDED(pILFrame2->EnumerateTypeParameters(&pTypeEnum)))
    {
        ULONG numTypes = 0;
        ToRelease<ICorDebugType> pCurrentTypeParam;

        while (SUCCEEDED(pTypeEnum->Next(1, &pCurrentTypeParam, &numTypes)) && numTypes == 1)
        {
            std::string name;
            GetTypeOfValue(pCurrentTypeParam, name);
            args.emplace_back(name);
            pCurrentTypeParam.Free();
        }
    }

    return S_OK;
}

HRESULT NameForTypeByType(ICorDebugType *pType, std::string &mdName)
{
    HRESULT Status;
    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));
    mdToken tk;
    IfFailRet(pClass->GetToken(&tk));
    std::list<std::string> args;
    AddGenericArgs(pType, args);
    return NameForTypeByToken(tk, pMD, mdName, &args);
}

HRESULT NameForTypeByValue(ICorDebugValue *pValue, std::string &mdName)
{
    HRESULT Status;
    ToRelease<ICorDebugValue2> iCorValue2;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
    ToRelease<ICorDebugType> iCorType;
    IfFailRet(iCorValue2->GetExactType(&iCorType));
    return NameForTypeByType(iCorType, mdName);
}

HRESULT NameForToken(mdToken mb,
                                  IMetaDataImport *pImport,
                                  std::string &mdName,
                                  bool bClassName,
                                  std::list<std::string> *args)
{
    mdName[0] = L'\0';
    if (TypeFromToken(mb) != mdtTypeDef
        && TypeFromToken(mb) != mdtFieldDef
        && TypeFromToken(mb) != mdtMethodDef
        && TypeFromToken(mb) != mdtMemberRef
        && TypeFromToken(mb) != mdtTypeRef)
    {
        //ExtOut("unsupported\n");
        return E_FAIL;
    }

    HRESULT hr = E_FAIL;

    WCHAR name[mdNameLen];
    if (TypeFromToken(mb) == mdtTypeDef)
    {
        hr = NameForTypeDef(mb, pImport, mdName, args);
    }
    else if (TypeFromToken(mb) ==  mdtFieldDef)
    {
        mdTypeDef mdClass;
        ULONG size;
        hr = pImport->GetMemberProps(mb, &mdClass,
                                        name, _countof(name), &size,
                                        NULL, NULL, NULL, NULL,
                                        NULL, NULL, NULL, NULL);
        if (SUCCEEDED(hr))
        {
            if (mdClass != mdTypeDefNil && bClassName)
            {
                hr = NameForTypeDef(mdClass, pImport, mdName, args);
                mdName += ".";
            }
            mdName += to_utf8(name/*, size*/);
        }
    }
    else if (TypeFromToken(mb) == mdtMethodDef)
    {
        mdTypeDef mdClass;
        ULONG size;
        hr = pImport->GetMethodProps(mb, &mdClass,
                                        name, _countof(name), &size,
                                        NULL, NULL, NULL, NULL, NULL);
        if (SUCCEEDED (hr))
        {
            if (mdClass != mdTypeDefNil && bClassName)
            {
                hr = NameForTypeDef(mdClass, pImport, mdName, args);
                mdName += ".";
            }
            mdName += to_utf8(name/*, size*/);
        }
    }
    else if (TypeFromToken(mb) == mdtMemberRef)
    {
        mdTypeDef mdClass;
        ULONG size;
        hr = pImport->GetMemberRefProps(mb, &mdClass,
                                        name, _countof(name), &size,
                                        NULL, NULL);
        if (SUCCEEDED (hr))
        {
            if (TypeFromToken(mdClass) == mdtTypeRef && bClassName)
            {
                hr = NameForTypeRef(mdClass, pImport, mdName);
                mdName += ".";
            }
            else if (TypeFromToken(mdClass) == mdtTypeDef && bClassName)
            {
                hr = NameForTypeDef(mdClass, pImport, mdName, args);
                mdName += ".";
            }
            // TODO TypeSpec
            mdName += to_utf8(name/*, size*/);
        }
    }
    else if (TypeFromToken(mb) == mdtTypeRef)
    {
        hr = NameForTypeRef(mb, pImport, mdName);
    }
    else
    {
        //ExtOut ("Unsupported token type\n");
        hr = E_FAIL;
    }

    if (SUCCEEDED(hr))
    {
        mdName = RenameToCSharp(mdName);
    }
    return hr;
}

HRESULT GetTypeOfValue(ICorDebugValue *pValue, std::string &output)
{
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugValue2> pValue2;
    if(SUCCEEDED(pValue->QueryInterface(IID_ICorDebugValue2, (void**) &pValue2)) && SUCCEEDED(pValue2->GetExactType(&pType)))
        return GetTypeOfValue(pType, output);
    else
        output = "<unknown>";

    return S_OK;
}

// From strike.cpp

HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &elementType, std::string &arrayType)
{
    if (pType == nullptr)
        return E_INVALIDARG;

    HRESULT Status = S_OK;

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));

    switch (corElemType)
    {
    //List of unsupported CorElementTypes:
    //ELEMENT_TYPE_END            = 0x0,
    //ELEMENT_TYPE_VAR            = 0x13,     // a class type variable VAR <U1>
    //ELEMENT_TYPE_GENERICINST    = 0x15,     // GENERICINST <generic type> <argCnt> <arg1> ... <argn>
    //ELEMENT_TYPE_TYPEDBYREF     = 0x16,     // TYPEDREF  (it takes no args) a typed reference to some other type
    //ELEMENT_TYPE_MVAR           = 0x1e,     // a method type variable MVAR <U1>
    //ELEMENT_TYPE_CMOD_REQD      = 0x1F,     // required C modifier : E_T_CMOD_REQD <mdTypeRef/mdTypeDef>
    //ELEMENT_TYPE_CMOD_OPT       = 0x20,     // optional C modifier : E_T_CMOD_OPT <mdTypeRef/mdTypeDef>
    //ELEMENT_TYPE_INTERNAL       = 0x21,     // INTERNAL <typehandle>
    //ELEMENT_TYPE_MAX            = 0x22,     // first invalid element type
    //ELEMENT_TYPE_MODIFIER       = 0x40,
    //ELEMENT_TYPE_SENTINEL       = 0x01 | ELEMENT_TYPE_MODIFIER, // sentinel for varargs
    //ELEMENT_TYPE_PINNED         = 0x05 | ELEMENT_TYPE_MODIFIER,
    //ELEMENT_TYPE_R4_HFA         = 0x06 | ELEMENT_TYPE_MODIFIER, // used only internally for R4 HFA types
    //ELEMENT_TYPE_R8_HFA         = 0x07 | ELEMENT_TYPE_MODIFIER, // used only internally for R8 HFA types
    default:
        {
            std::ostringstream ss;
            ss << "(Unhandled CorElementType: 0x" << std::hex << corElemType << ")";
            elementType = ss.str();
        }
        break;

    case ELEMENT_TYPE_VALUETYPE:
    case ELEMENT_TYPE_CLASS:
        {
            std::ostringstream ss;
            //Defaults in case we fail...
            elementType = (corElemType == ELEMENT_TYPE_VALUETYPE) ? "struct" : "class";

            mdTypeDef typeDef;
            ToRelease<ICorDebugClass> pClass;
            if(SUCCEEDED(pType->GetClass(&pClass)) && SUCCEEDED(pClass->GetToken(&typeDef)))
            {
                ToRelease<ICorDebugModule> pModule;
                IfFailRet(pClass->GetModule(&pModule));

                ToRelease<IUnknown> pMDUnknown;
                ToRelease<IMetaDataImport> pMD;
                IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
                IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

                std::string name;
                std::list<std::string> args;
                AddGenericArgs(pType, args);
                if(SUCCEEDED(NameForToken(TokenFromRid(typeDef, mdtTypeDef), pMD, name, false, &args)))
                {
                    static const Utility::string_view nullablePattern = "System.Nullable<";
                    if (name.rfind(nullablePattern, 0) == 0)
                    {
                        ss << name.substr(nullablePattern.size(), name.rfind(">") - nullablePattern.size()) << "?";
                    }
                    else
                        ss << name;
                }
            }
            elementType = ss.str();
            return S_OK;
        }
        break;
    case ELEMENT_TYPE_VOID:
        elementType = "void";
        break;
    case ELEMENT_TYPE_BOOLEAN:
        elementType = "bool";
        break;
    case ELEMENT_TYPE_CHAR:
        elementType = "char";
        break;
    case ELEMENT_TYPE_I1:
        elementType = "sbyte";
        break;
    case ELEMENT_TYPE_U1:
        elementType = "byte";
        break;
    case ELEMENT_TYPE_I2:
        elementType = "short";
        break;
    case ELEMENT_TYPE_U2:
        elementType = "ushort";
        break;
    case ELEMENT_TYPE_I4:
        elementType = "int";
        break;
    case ELEMENT_TYPE_U4:
        elementType = "uint";
        break;
    case ELEMENT_TYPE_I8:
        elementType = "long";
        break;
    case ELEMENT_TYPE_U8:
        elementType = "ulong";
        break;
    case ELEMENT_TYPE_R4:
        elementType = "float";
        break;
    case ELEMENT_TYPE_R8:
        elementType = "double";
        break;
    case ELEMENT_TYPE_OBJECT:
        elementType = "object";
        break;
    case ELEMENT_TYPE_STRING:
        elementType = "string";
        break;
    case ELEMENT_TYPE_I:
        elementType = "IntPtr";
        break;
    case ELEMENT_TYPE_U:
        elementType = "UIntPtr";
        break;
    case ELEMENT_TYPE_SZARRAY:
    case ELEMENT_TYPE_ARRAY:
    case ELEMENT_TYPE_BYREF:
    case ELEMENT_TYPE_PTR:
        {
            std::string subElementType;
            std::string subArrayType;
            ToRelease<ICorDebugType> pFirstParameter;
            if(SUCCEEDED(pType->GetFirstTypeParameter(&pFirstParameter)))
                GetTypeOfValue(pFirstParameter, subElementType, subArrayType);
            else
                subElementType = "<unknown>";

            elementType = subElementType;

            switch(corElemType)
            {
            case ELEMENT_TYPE_SZARRAY:
                arrayType = "[]" + subArrayType;
                return S_OK;
            case ELEMENT_TYPE_ARRAY:
                {
                    std::ostringstream ss;
                    ULONG32 rank = 0;
                    pType->GetRank(&rank);
                    ss << "[";
                    for(ULONG32 i = 0; i < rank - 1; i++)
                        ss << ",";
                    ss << "]";
                    arrayType = ss.str() + subArrayType;
                }
                return S_OK;
            case ELEMENT_TYPE_BYREF:
                arrayType = subArrayType; // + "&";
                return S_OK;
            case ELEMENT_TYPE_PTR:
                arrayType = subArrayType + "*";
                return S_OK;
            default:
                // note we can never reach here as this is a nested switch
                // and corElemType can only be one of the values above
                break;
            }
        }
        break;
    case ELEMENT_TYPE_FNPTR:
        elementType = "*(...)";
        break;
    case ELEMENT_TYPE_TYPEDBYREF:
        elementType = "typedbyref";
        break;
    }
    return S_OK;
}

// From sildasm.cpp
static PCCOR_SIGNATURE NameForTypeSig(PCCOR_SIGNATURE typePtr, const std::vector<std::string> &args,
                                      IMetaDataImport *pImport, std::string &out, std::string &appendix)
{
    mdToken tk;
    const char* str;
    int typ;
    std::string tmp;
    int n;

    switch(typ = CorSigUncompressElementType(typePtr)) {
        case ELEMENT_TYPE_VOID          :
            out = "void"; break;
        case ELEMENT_TYPE_BOOLEAN       :
            out = "bool"; break;
        case ELEMENT_TYPE_CHAR          :
            out = "char"; break;
        case ELEMENT_TYPE_I1            :
            out = "sbyte"; break;
        case ELEMENT_TYPE_U1            :
            out = "byte"; break;
        case ELEMENT_TYPE_I2            :
            out = "short"; break;
        case ELEMENT_TYPE_U2            :
            out = "ushort"; break;
        case ELEMENT_TYPE_I4            :
            out = "int"; break;
        case ELEMENT_TYPE_U4            :
            out = "uint"; break;
        case ELEMENT_TYPE_I8            :
            out = "long"; break;
        case ELEMENT_TYPE_U8            :
            out = "ulong"; break;
        case ELEMENT_TYPE_R4            :
            out = "float"; break;
        case ELEMENT_TYPE_R8            :
            out = "double"; break;
        case ELEMENT_TYPE_U             :
            out = "UIntPtr"; break;
        case ELEMENT_TYPE_I             :
            out = "IntPtr"; break;
        case ELEMENT_TYPE_OBJECT        :
            out = "object"; break;
        case ELEMENT_TYPE_STRING        :
            out = "string"; break;
        case ELEMENT_TYPE_TYPEDBYREF        :
            out = "typedref"; break;

        case ELEMENT_TYPE_VALUETYPE    :
        case ELEMENT_TYPE_CLASS        :
            {
                typePtr += CorSigUncompressToken(typePtr, &tk);
                NameForToken(tk, pImport, out, true, nullptr);
            }
            break;

        case ELEMENT_TYPE_SZARRAY    :
            {
                std::string subAppendix;
                typePtr = NameForTypeSig(typePtr, args, pImport, out, subAppendix);
                appendix = "[]" + subAppendix;
            }
            break;

        case ELEMENT_TYPE_ARRAY       :
            {
                std::string subAppendix;
                typePtr = NameForTypeSig(typePtr, args, pImport, out, subAppendix);
                std::string newAppendix;
                unsigned rank = CorSigUncompressData(typePtr);
                // <TODO> what is the syntax for the rank 0 case? </TODO>
                if (rank == 0) {
                    newAppendix += "[BAD: RANK == 0!]";
                }
                else {
                    std::vector<int> lowerBounds(rank, 0);
                    std::vector<int> sizes(rank, 0);

                    unsigned numSizes = CorSigUncompressData(typePtr);
                    assert(numSizes <= rank);
                        unsigned i;
                    for(i =0; i < numSizes; i++)
                        sizes[i] = CorSigUncompressData(typePtr);

                    unsigned numLowBounds = CorSigUncompressData(typePtr);
                    assert(numLowBounds <= rank);
                    for(i = 0; i < numLowBounds; i++)
                        typePtr += CorSigUncompressSignedInt(typePtr,&lowerBounds[i]);

                    newAppendix += '[';
                    if (rank == 1 && numSizes == 0 && numLowBounds == 0)
                        newAppendix += "..";
                    else
                    {
                        for(i = 0; i < rank; i++)
                        {
                                 //if (sizes[i] != 0 || lowerBounds[i] != 0)
                            // {
                            //     if (i < numSizes && lowerBounds[i] == 0)
                            //         out += std::to_string(sizes[i]);
                            //     else
                            //     {
                            //         if(i < numLowBounds)
                            //         {
                            //             newAppendix +=  std::to_string(lowerBounds[i]);
                            //             newAppendix += "..";
                            //             if (/*sizes[i] != 0 && */i < numSizes)
                            //                 newAppendix += std::to_string(lowerBounds[i] + sizes[i] - 1);
                            //         }
                            //     }
                            // }
                            if (i < rank-1)
                                newAppendix += ',';
                        }
                    }
                    newAppendix += ']';
                }
                appendix = newAppendix + subAppendix;
            }
            break;

        case ELEMENT_TYPE_VAR        :
            n  = CorSigUncompressData(typePtr);
            out = n < int(args.size()) ? args.at(n) : "!" + std::to_string(n);
            break;

        case ELEMENT_TYPE_MVAR        :
            out += "!!";
            n  = CorSigUncompressData(typePtr);
            out += std::to_string(n);
            break;

        case ELEMENT_TYPE_FNPTR :
            out = "method ";
            out += "METHOD"; // was: typePtr = PrettyPrintSignature(typePtr, 0x7FFF, "*", out, pIMDI, NULL);
            break;

        case ELEMENT_TYPE_GENERICINST :
            {
                //typePtr = NameForTypeSig(typePtr, args, pImport, out, appendix);
                CorElementType underlyingType;
                typePtr += CorSigUncompressElementType(typePtr, &underlyingType);
                typePtr += CorSigUncompressToken(typePtr, &tk);

                std::list<std::string> genericArgs;

                unsigned numArgs = CorSigUncompressData(typePtr);
                while(numArgs--)
                {
                    std::string genType;
                    std::string genTypeAppendix;
                    typePtr = NameForTypeSig(typePtr, args, pImport, genType, genTypeAppendix);
                    genericArgs.push_back(genType + genTypeAppendix);
                }
                NameForToken(tk, pImport, out, true, &genericArgs);
            }
            break;

        case ELEMENT_TYPE_PINNED	:
            str = " pinned"; goto MODIFIER;
        case ELEMENT_TYPE_PTR           :
            str = "*"; goto MODIFIER;
        case ELEMENT_TYPE_BYREF         :
            str = "&"; goto MODIFIER;
        MODIFIER:
            {
                std::string subAppendix;
                typePtr = NameForTypeSig(typePtr, args, pImport, out, subAppendix);
                appendix = str + subAppendix;
            }
            break;

        default:
        case ELEMENT_TYPE_SENTINEL      :
        case ELEMENT_TYPE_END           :
            //assert(!"Unknown Type");
            if(typ)
            {
                out = "/* UNKNOWN TYPE (0x%X)*/" + std::to_string(typ);
            }
            break;
    } // end switch

    return typePtr;
}

void NameForTypeSig(
    PCCOR_SIGNATURE typePtr,
    ICorDebugType *enclosingType,
    IMetaDataImport *pImport,
    std::string &typeName)
{
    // Gather generic arguments from enclosing type
    std::vector<std::string> args;
    ToRelease<ICorDebugTypeEnum> pTypeEnum;

    if (SUCCEEDED(enclosingType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ULONG fetched = 0;
        ToRelease<ICorDebugType> pCurrentTypeParam;

        while (SUCCEEDED(pTypeEnum->Next(1, &pCurrentTypeParam, &fetched)) && fetched == 1)
        {
            std::string name;
            GetTypeOfValue(pCurrentTypeParam, name);
            args.emplace_back(name);
            pCurrentTypeParam.Free();
        }
    }

    std::string out;
    std::string appendix;
    NameForTypeSig(typePtr, args, pImport, out, appendix);
    typeName = out + appendix;
}

HRESULT GetTypeOfValue(ICorDebugType *pType, std::string &output)
{
    HRESULT Status;
    std::string elementType;
    std::string arrayType;
    IfFailRet(GetTypeOfValue(pType, elementType, arrayType));
    output = elementType + arrayType;
    return S_OK;
}

HRESULT GetTypeAndMethod(ICorDebugFrame *pFrame, std::string &typeName, std::string &methodName)
{
    HRESULT Status;

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugModule> pModule;
    mdMethodDef methodDef;
    IfFailRet(pFunction->GetClass(&pClass));
    IfFailRet(pFunction->GetModule(&pModule));
    IfFailRet(pFunction->GetToken(&methodDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;

    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));

    mdTypeDef memTypeDef;
    ULONG nameLen;
    DWORD flags;
    PCCOR_SIGNATURE pbSigBlob;
    ULONG ulSigBlob;
    ULONG ulCodeRVA;
    ULONG ulImplFlags;

    WCHAR szFunctionName[mdNameLen] = {0};

    ToRelease<IMetaDataImport2> pMD2;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport2, (LPVOID*) &pMD2));

    IfFailRet(pMD->GetMethodProps(methodDef, &memTypeDef,
                                  szFunctionName, _countof(szFunctionName), &nameLen,
                                  &flags, &pbSigBlob, &ulSigBlob, &ulCodeRVA, &ulImplFlags));

    std::string funcName = to_utf8(szFunctionName/*, nameLen*/);

    ULONG methodGenericsCount = 0;
    HCORENUM hEnum = NULL;
    mdGenericParam gp;
    ULONG fetched;
    while (SUCCEEDED(pMD2->EnumGenericParams(&hEnum, methodDef, &gp, 1, &fetched)) && fetched == 1)
    {
        methodGenericsCount++;
    }
    pMD2->CloseEnum(hEnum);

    if (methodGenericsCount > 0)
    {
        std::ostringstream ss;
        ss << funcName << '`' << methodGenericsCount;
        funcName = ss.str();
    }

    std::list<std::string> args;
    AddGenericArgs(pFrame, args);

    if (memTypeDef != mdTypeDefNil)
    {
        if (FAILED(NameForTypeDef(memTypeDef, pMD, typeName, &args)))
            typeName = "";
    }

    methodName = ConsumeGenericArgs(funcName, args);

    return S_OK;
}

HRESULT GetMethodName(ICorDebugFrame *pFrame, std::string &output)
{
    HRESULT Status;

    std::string typeName;
    std::string methodName;

    std::ostringstream ss;
    IfFailRet(GetTypeAndMethod(pFrame, typeName, methodName));
    if (!typeName.empty())
        ss << typeName << ".";
    ss << methodName << "()";

    output = ss.str();
    return S_OK;
}

} // namespace TypePrinter

} // namespace netcoredbg
