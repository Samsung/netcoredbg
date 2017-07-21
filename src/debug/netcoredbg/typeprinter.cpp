#include "common.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <list>
#include <iomanip>


typedef char * LPCUTF8;
typedef uintptr_t TADDR;
#include "sos_md.h"

#include "typeprinter.h"

#include "cputil.h"

// <TODO> Get rid of these!  Don't use them any more!</TODO>
#define MAX_CLASSNAME_LENGTH    1024
#define MAX_NAMESPACE_LENGTH    1024


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

    std::stringstream ss;
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

// From metadata.cpp

/**********************************************************************\
* Routine Description:                                                 *
*                                                                      *
*    This function is called to find the name of a TypeDef using       *
*    metadata API.                                                     *
*                                                                      *
\**********************************************************************/
// Caller should guard against exception
HRESULT TypePrinter::NameForTypeDef(
    mdTypeDef tkTypeDef,
    IMetaDataImport *pImport,
    std::string &mdName,
    std::list<std::string> &args)
{
    HRESULT Status;
    DWORD flags;
    WCHAR name[mdNameLen];
    ULONG nameLen;

    IfFailRet(pImport->GetTypeDefProps(tkTypeDef, name, _countof(name), &nameLen, &flags, NULL));
    mdName = to_utf8(name/*, nameLen*/);

    if (!IsTdNested(flags))
    {
        mdName = ConsumeGenericArgs(mdName, args);
        return S_OK;
    }

    mdTypeDef tkEnclosingClass;
    IfFailRet(pImport->GetNestedClassProps(tkTypeDef, &tkEnclosingClass));

    std::string enclosingName;
    IfFailRet(NameForTypeDef(tkEnclosingClass, pImport, enclosingName, args));

    mdName = enclosingName + "." + ConsumeGenericArgs(mdName, args);

    return S_OK;
}

HRESULT TypePrinter::NameForToken(mdTypeDef mb,
                                  IMetaDataImport *pImport,
                                  std::string &mdName,
                                  bool bClassName,
                                  std::list<std::string> &args)
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

    PAL_CPP_TRY
    {
        WCHAR name[MAX_CLASSNAME_LENGTH];
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
                    ToRelease<IMDInternalImport> pMDI;
                    hr = GetMDInternalFromImport(pImport, &pMDI);
                    if (SUCCEEDED(hr))
                    {
                        LPCSTR sznamespace = 0;
                        LPCSTR szname = 0;
                        if (SUCCEEDED(pMDI->GetNameOfTypeRef(mdClass, &sznamespace, &szname)))
                            mdName = std::string(sznamespace) + "." + std::string(szname) + ".";
                    }
                }
                else if (TypeFromToken(mdClass) == mdtTypeDef && bClassName)
                {
                    hr = NameForTypeDef(mdClass, pImport, mdName, args);
                    mdName += ".";
                }
                mdName += to_utf8(name/*, size*/);
            }
        }
        else if (TypeFromToken(mb) == mdtTypeRef)
        {
            ToRelease<IMDInternalImport> pMDI;
            hr = GetMDInternalFromImport(pImport, &pMDI);
            if (SUCCEEDED(hr))
            {
                LPCSTR sznamespace = 0;
                LPCSTR szname = 0;
                if (SUCCEEDED(pMDI->GetNameOfTypeRef(mdtTypeRef, &sznamespace, &szname)))
                    mdName = std::string(sznamespace) + "." + std::string(szname);
            }
        }
        else
        {
            //ExtOut ("Unsupported token type\n");
            hr = E_FAIL;
        }
    }
    PAL_CPP_CATCH_ALL
    {
            hr = E_FAIL;
    }
    PAL_CPP_ENDTRY
    return hr;
}

HRESULT TypePrinter::AddGenericArgs(ICorDebugType *pType, std::list<std::string> &args)
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
        }
    }

    return S_OK;
}

HRESULT TypePrinter::AddGenericArgs(ICorDebugFrame *pFrame, std::list<std::string> &args)
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
        }
    }

    return S_OK;
}

HRESULT TypePrinter::GetTypeOfValue(ICorDebugValue *pValue, std::string &output)
{
    HRESULT Status = S_OK;

    CorElementType corElemType;
    IfFailRet(pValue->GetType(&corElemType));

    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugValue2> pValue2;
    if(SUCCEEDED(pValue->QueryInterface(IID_ICorDebugValue2, (void**) &pValue2)) && SUCCEEDED(pValue2->GetExactType(&pType)))
        return GetTypeOfValue(pType, output);
    else
        output = "<unknown>";

    return S_OK;
}

// From strike.cpp

HRESULT TypePrinter::GetTypeOfValue(ICorDebugType *pType, std::string &elementType, std::string &arrayType)
{
    HRESULT Status = S_OK;

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));

    switch (corElemType)
    {
    //List of unsupported CorElementTypes:
    //ELEMENT_TYPE_END            = 0x0,
    //ELEMENT_TYPE_VAR            = 0x13,     // a class type variable VAR <U1>
    //ELEMENT_TYPE_GENERICINST    = 0x15,     // GENERICINST <generic type> <argCnt> <arg1> ... <argn>
    //ELEMENT_TYPE_TYPEDBYREF     = 0x16,     // TYPEDREF  (it takes no args) a typed referece to some other type
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
            std::stringstream ss;
            ss << "(Unhandled CorElementType: 0x" << std::hex << corElemType << ")";
            elementType = ss.str();
        }
        break;

    case ELEMENT_TYPE_VALUETYPE:
    case ELEMENT_TYPE_CLASS:
        {
            std::stringstream ss;
            //Defaults in case we fail...
            elementType = (corElemType == ELEMENT_TYPE_VALUETYPE) ? "struct" : "class";

            std::list<std::string> args;
            AddGenericArgs(pType, args);

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

                if(SUCCEEDED(NameForToken(TokenFromRid(typeDef, mdtTypeDef), pMD, name, false, args)))
                {
                    if (name == "System.Decimal")
                        ss << "decimal";
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
                    std::stringstream ss;
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

HRESULT TypePrinter::GetTypeOfValue(ICorDebugType *pType, std::string &output)
{
    HRESULT Status;
    std::string elementType;
    std::string arrayType;
    IfFailRet(GetTypeOfValue(pType, elementType, arrayType));
    output = elementType + arrayType;
    return S_OK;
}

HRESULT TypePrinter::GetMethodName(ICorDebugFrame *pFrame, std::string &output)
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

    HRESULT hr;
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

    hr = pMD->GetMethodProps(methodDef, &memTypeDef,
                             szFunctionName, _countof(szFunctionName), &nameLen,
                             &flags, &pbSigBlob, &ulSigBlob, &ulCodeRVA, &ulImplFlags);

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
        std::stringstream ss;
        ss << funcName << '`' << methodGenericsCount;
        funcName = ss.str();
    }

    std::list<std::string> args;
    AddGenericArgs(pFrame, args);

    std::stringstream ss;
    if (memTypeDef != mdTypeDefNil)
    {
        std::string name;
        hr = NameForTypeDef(memTypeDef, pMD, name, args);
        if (SUCCEEDED(hr))
        {
            ss << name << ".";
        }
    }

    ss << ConsumeGenericArgs(funcName, args);
    ss << "()";

    output = ss.str();
    return S_OK;
}
