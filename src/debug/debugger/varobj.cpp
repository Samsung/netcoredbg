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
#include <iomanip>

#include "torelease.h"
#include "arrayholder.h"

// Modules
HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue);


// From strike.cpp
static HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = NULL)
{
    HRESULT Status = S_OK;
    *ppOutputValue = NULL;
    if (pIsNull != NULL) *pIsNull = FALSE;

    ToRelease<ICorDebugReferenceValue> pReferenceValue;
    Status = pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue);
    if (SUCCEEDED(Status))
    {
        BOOL isNull = FALSE;
        IfFailRet(pReferenceValue->IsNull(&isNull));
        if(!isNull)
        {
            ToRelease<ICorDebugValue> pDereferencedValue;
            IfFailRet(pReferenceValue->Dereference(&pDereferencedValue));
            return DereferenceAndUnboxValue(pDereferencedValue, ppOutputValue);
        }
        else
        {
            if(pIsNull != NULL) *pIsNull = TRUE;
            *ppOutputValue = pValue;
            (*ppOutputValue)->AddRef();
            return S_OK;
        }
    }

    ToRelease<ICorDebugBoxValue> pBoxedValue;
    Status = pValue->QueryInterface(IID_ICorDebugBoxValue, (LPVOID*) &pBoxedValue);
    if (SUCCEEDED(Status))
    {
        ToRelease<ICorDebugObjectValue> pUnboxedValue;
        IfFailRet(pBoxedValue->GetObject(&pUnboxedValue));
        return DereferenceAndUnboxValue(pUnboxedValue, ppOutputValue);
    }
    *ppOutputValue = pValue;
    (*ppOutputValue)->AddRef();
    return S_OK;
}

static BOOL IsEnum(ICorDebugValue * pInputValue)
{
    return FALSE;
}

static HRESULT PrintStringValue(ICorDebugValue * pValue, std::string &output)
{
    HRESULT Status;

    ToRelease<ICorDebugStringValue> pStringValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugStringValue, (LPVOID*) &pStringValue));

    ULONG32 cchValue;
    IfFailRet(pStringValue->GetLength(&cchValue));
    cchValue++;         // Allocate one more for null terminator

    ArrayHolder<WCHAR> str = new WCHAR[cchValue];

    ULONG32 cchValueReturned;
    IfFailRet(pStringValue->GetString(
        cchValue,
        &cchValueReturned,
        str));

    ULONG32 cstrLen = cchValue * 2;
    ArrayHolder<char> cstr = new char[cstrLen];

    WideCharToMultiByte(CP_UTF8, 0, str, cchValue, cstr, cstrLen, NULL, NULL);

    output = cstr;

    return S_OK;
}

HRESULT PrintValue(ICorDebugValue *pInputValue, ICorDebugILFrame * pILFrame, IMetaDataImport * pMD, std::string &output)
{
    HRESULT Status;

    BOOL isNull = TRUE;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if(isNull)
    {
        output = "null";
        return S_OK;
    }

    ULONG32 cbSize;
    IfFailRet(pValue->GetSize(&cbSize));
    ArrayHolder<BYTE> rgbValue = new (std::nothrow) BYTE[cbSize];
    if (rgbValue == NULL)
    {
        return E_OUTOFMEMORY;
    }

    memset(rgbValue.GetPtr(), 0, cbSize * sizeof(BYTE));

    CorElementType corElemType;
    IfFailRet(pValue->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
    {
        std::string raw_str;
        IfFailRet(PrintStringValue(pValue, raw_str));

        std::stringstream ss;
        ss << "\\\"" << raw_str << "\\\"";
        output = ss.str();
        return S_OK;
    }

    if (corElemType == ELEMENT_TYPE_SZARRAY)
    {
        output = "<ELEMENT_TYPE_SZARRAY>";
        //return PrintSzArrayValue(pValue, pILFrame, pMD);
        return S_OK;
    }

    ToRelease<ICorDebugGenericValue> pGenericValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
    IfFailRet(pGenericValue->GetValue((LPVOID) &(rgbValue[0])));

    if(IsEnum(pValue))
    {
        output = "<enum>";
        //Status = PrintEnumValue(pValue, rgbValue);
        return Status;
    }

    std::stringstream ss;

    switch (corElemType)
    {
    default:
        ss << "(Unhandled CorElementType: 0x" << std::hex << corElemType << ")";
        break;

    case ELEMENT_TYPE_PTR:
        ss << "<pointer>";
        break;

    case ELEMENT_TYPE_FNPTR:
        {
            CORDB_ADDRESS addr = 0;
            ToRelease<ICorDebugReferenceValue> pReferenceValue = NULL;
            if(SUCCEEDED(pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue)))
                pReferenceValue->GetValue(&addr);
            ss << "<function pointer 0x" << std::hex << addr << ">";
        }
        break;

    case ELEMENT_TYPE_VALUETYPE:
    case ELEMENT_TYPE_CLASS:
        CORDB_ADDRESS addr;
        if(SUCCEEDED(pValue->GetAddress(&addr)))
        {
            ss << " @ 0x" << std::hex << addr;
        }
        else
        {
            ss << "<failed to get address>";
        }
        //ProcessFields(pValue, NULL, pILFrame, indent + 1, varToExpand, currentExpansion, currentExpansionSize, currentFrame);
        break;

    case ELEMENT_TYPE_BOOLEAN:
        ss << (rgbValue[0] == 0 ? "false" : "true");
        break;

    case ELEMENT_TYPE_CHAR:
        {
            WCHAR ws[2] = W("\0");
            ws[0] = *(WCHAR *) &(rgbValue[0]);
            char printableVal[10] = {0};
            WideCharToMultiByte(CP_UTF8, 0, ws, 2, printableVal, _countof(printableVal), NULL, NULL);

            ss << (unsigned int)ws[0] << " '" << printableVal << "'";
        }
        break;

    case ELEMENT_TYPE_I1:
        ss << *(char*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U1:
        ss << *(unsigned char*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_I2:
        ss << *(short*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U2:
        ss << *(unsigned short*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_I:
        ss << *(int*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U:
        ss << *(unsigned int*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_I4:
        ss << *(int*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U4:
        ss << *(unsigned int*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_I8:
        ss << *(__int64*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U8:
        ss << *(unsigned __int64*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_R4:
        ss << *(float*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_R8:
        ss << *(double*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_OBJECT:
        ss << "object";
        break;

        // TODO: The following corElementTypes are not yet implemented here.  Array
        // might be interesting to add, though the others may be of rather limited use:
        // ELEMENT_TYPE_ARRAY          = 0x14,     // MDARRAY <type> <rank> <bcount> <bound1> ... <lbcount> <lb1> ...
        //
        // ELEMENT_TYPE_GENERICINST    = 0x15,     // GENERICINST <generic type> <argCnt> <arg1> ... <argn>
    }

    output = ss.str();
    return S_OK;
}

HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output)
{
    bool printValues = true;
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

            ss << sep << "{name=\"" << cParamName << "\"";
            if (printValues)
            {
                std::string strVal;
                if (SUCCEEDED(PrintValue(pValue, pILFrame, pMD, strVal)))
                    ss << ",value=\"" << strVal << "\"";
            }
            ss << "}";
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

            ss << sep << "{name=\"" << paramName << "\"";
            if (printValues)
            {
                std::string strVal;
                if (SUCCEEDED(PrintValue(pValue, pILFrame, pMD, strVal)))
                    ss << ",value=\"" << strVal << "\"";
            }
            ss << "}";
            sep = ",";
        }
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}
