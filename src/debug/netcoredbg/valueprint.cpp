#include "common.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <list>
#include <iomanip>

#include "cputil.h"
#include "typeprinter.h"

// From strike.cpp
HRESULT DereferenceAndUnboxValue(ICorDebugValue * pValue, ICorDebugValue** ppOutputValue, BOOL * pIsNull = NULL)
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
            return DereferenceAndUnboxValue(pDereferencedValue, ppOutputValue, pIsNull);
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
        return DereferenceAndUnboxValue(pUnboxedValue, ppOutputValue, pIsNull);
    }
    *ppOutputValue = pValue;
    (*ppOutputValue)->AddRef();
    return S_OK;
}

static bool IsEnum(ICorDebugValue *pInputValue)
{
    ToRelease<ICorDebugValue> pValue;
    if (FAILED(DereferenceAndUnboxValue(pInputValue, &pValue, nullptr))) return false;

    std::string baseTypeName;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugType> pBaseType;

    if (FAILED(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2))) return false;
    if (FAILED(pValue2->GetExactType(&pType))) return false;
    if (FAILED(pType->GetBase(&pBaseType)) || pBaseType == nullptr) return false;
    if (FAILED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName))) return false;

    return baseTypeName == "System.Enum";
}

static HRESULT PrintEnumValue(ICorDebugValue* pInputValue, BYTE* enumValue, std::string &output)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugValue> pValue;
    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, NULL));

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    IfFailRet(pValue2->GetExactType(&pType));
    IfFailRet(pType->GetClass(&pClass));
    IfFailRet(pClass->GetModule(&pModule));
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));


    //First, we need to figure out the underlying enum type so that we can correctly type cast the raw values of each enum constant
    //We get that from the non-static field of the enum variable (I think the field is called __value or something similar)
    ULONG numFields = 0;
    HCORENUM fEnum = NULL;
    mdFieldDef fieldDef;
    CorElementType enumUnderlyingType = ELEMENT_TYPE_END;
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        DWORD             fieldAttr = 0;
        PCCOR_SIGNATURE   pSignatureBlob = NULL;
        ULONG             sigBlobLength = 0;
        if(SUCCEEDED(pMD->GetFieldProps(fieldDef, NULL, NULL, 0, NULL, &fieldAttr, &pSignatureBlob, &sigBlobLength, NULL, NULL, NULL)))
        {
            if((fieldAttr & fdStatic) == 0)
            {
                CorSigUncompressCallingConv(pSignatureBlob);
                enumUnderlyingType = CorSigUncompressElementType(pSignatureBlob);
                break;
            }
        }
    }
    pMD->CloseEnum(fEnum);

    std::stringstream ss;
    const char *sep = "";

    //Now that we know the underlying enum type, let's decode the enum variable into OR-ed, human readable enum contants
    fEnum = NULL;
    bool isFirst = true;
    ULONG64 remainingValue = *((ULONG64*)enumValue);
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG             nameLen = 0;
        DWORD             fieldAttr = 0;
        WCHAR             mdName[mdNameLen];
        WCHAR             typeName[mdNameLen];
        UVCP_CONSTANT     pRawValue = NULL;
        ULONG             rawValueLength = 0;
        if(SUCCEEDED(pMD->GetFieldProps(fieldDef, NULL, mdName, mdNameLen, &nameLen, &fieldAttr, NULL, NULL, NULL, &pRawValue, &rawValueLength)))
        {
            DWORD enumValueRequiredAttributes = fdPublic | fdStatic | fdLiteral | fdHasDefault;
            if((fieldAttr & enumValueRequiredAttributes) != enumValueRequiredAttributes)
                continue;

            ULONG64 currentConstValue = 0;
            switch (enumUnderlyingType)
            {
                case ELEMENT_TYPE_CHAR:
                case ELEMENT_TYPE_I1:
                    currentConstValue = (ULONG64)(*((CHAR*)pRawValue));
                    break;
                case ELEMENT_TYPE_U1:
                    currentConstValue = (ULONG64)(*((BYTE*)pRawValue));
                    break;
                case ELEMENT_TYPE_I2:
                    currentConstValue = (ULONG64)(*((SHORT*)pRawValue));
                    break;
                case ELEMENT_TYPE_U2:
                    currentConstValue = (ULONG64)(*((USHORT*)pRawValue));
                    break;
                case ELEMENT_TYPE_I4:
                    currentConstValue = (ULONG64)(*((INT32*)pRawValue));
                    break;
                case ELEMENT_TYPE_U4:
                    currentConstValue = (ULONG64)(*((UINT32*)pRawValue));
                    break;
                case ELEMENT_TYPE_I8:
                    currentConstValue = (ULONG64)(*((LONG*)pRawValue));
                    break;
                case ELEMENT_TYPE_U8:
                    currentConstValue = (ULONG64)(*((ULONG*)pRawValue));
                    break;
                case ELEMENT_TYPE_I:
                    currentConstValue = (ULONG64)(*((int*)pRawValue));
                    break;
                case ELEMENT_TYPE_U:
                case ELEMENT_TYPE_R4:
                case ELEMENT_TYPE_R8:
                // Technically U and the floating-point ones are options in the CLI, but not in the CLS or C#, so these are NYI
                default:
                    currentConstValue = 0;
            }

            if((currentConstValue == remainingValue) || ((currentConstValue != 0) && ((currentConstValue & remainingValue) == currentConstValue)))
            {
                remainingValue &= ~currentConstValue;

                ss << sep;
                sep = " | ";
                ss << to_utf8(mdName);
            }
        }
    }
    pMD->CloseEnum(fEnum);

    output = ss.str();

    return S_OK;
}

static HRESULT GetUIntValue(ICorDebugValue *pInputValue, unsigned int &value)
{
    HRESULT Status;

    BOOL isNull = TRUE;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if(isNull)
        return E_FAIL;

    ULONG32 cbSize;
    IfFailRet(pValue->GetSize(&cbSize));
    if (cbSize != sizeof(int))
        return E_FAIL;

    BYTE rgbValue[sizeof(int)] = {0};

    ToRelease<ICorDebugGenericValue> pGenericValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
    IfFailRet(pGenericValue->GetValue((LPVOID) &(rgbValue[0])));

    CorElementType corElemType;
    IfFailRet(pValue->GetType(&corElemType));

    switch (corElemType)
    {
    default:
        return E_FAIL;
    case ELEMENT_TYPE_I4:
    case ELEMENT_TYPE_U4:
        value = *(unsigned int*) &(rgbValue[0]);
        return S_OK;
    }
    return E_FAIL;
}

static HRESULT GetDecimalFields(ICorDebugValue *pValue,
                                unsigned int &hi,
                                unsigned int &mid,
                                unsigned int &lo,
                                unsigned int &flags)
{
    HRESULT Status = S_OK;

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    IfFailRet(pValue2->GetExactType(&pType));
    IfFailRet(pType->GetClass(&pClass));
    IfFailRet(pClass->GetModule(&pModule));
    IfFailRet(pClass->GetToken(&currentTypeDef));
    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    bool has_hi = false;
    bool has_mid = false;
    bool has_lo = false;
    bool has_flags = false;

    ULONG numFields = 0;
    HCORENUM fEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        if(SUCCEEDED(pMD->GetFieldProps(fieldDef, NULL, mdName, mdNameLen, &nameLen, &fieldAttr, NULL, NULL, NULL, NULL, NULL)))
        {
            if(fieldAttr & fdLiteral)
                continue;
            if (fieldAttr & fdStatic)
                continue;

            ToRelease<ICorDebugValue> pFieldVal;
            ToRelease<ICorDebugObjectValue> pObjValue;
            IfFailRet(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue));
            IfFailRet(pObjValue->GetFieldValue(pClass, fieldDef, &pFieldVal));

            std::string name = to_utf8(mdName /*, nameLen*/);

            if (name == "hi")
            {
                IfFailRet(GetUIntValue(pFieldVal, hi));
                has_hi = true;
            }
            else if (name == "mid")
            {
                IfFailRet(GetUIntValue(pFieldVal, mid));
                has_mid = true;
            } else if (name == "lo")
            {
                IfFailRet(GetUIntValue(pFieldVal, lo));
                has_lo = true;
            }
            else if (name == "flags")
            {
                IfFailRet(GetUIntValue(pFieldVal, flags));
                has_flags = true;
            }
        }
    }
    pMD->CloseEnum(fEnum);

    return (has_hi && has_mid && has_lo && has_flags ? S_OK : E_FAIL);
}

static inline uint64_t Make_64(uint32_t h, uint32_t l) { uint64_t v = h; v <<= 32; v |= l; return v; }
static inline uint32_t Lo_32(uint64_t v) { return static_cast<uint32_t>(v); }

bool uint96_is_zero(const uint32_t *v) { return v[0] == 0 && v[1] == 0 && v[2] == 0; }

static void udivrem96(uint32_t *divident, uint32_t divisor, uint32_t &remainder)
{
    remainder = 0;
    for (int i = 2; i >= 0; i--)
    {
        uint64_t partial_dividend = Make_64(remainder, divident[i]);
        if (partial_dividend == 0) {
            divident[i] = 0;
            remainder = 0;
        } else if (partial_dividend < divisor) {
            divident[i] = 0;
            remainder = Lo_32(partial_dividend);
        } else if (partial_dividend == divisor) {
            divident[i] = 1;
            remainder = 0;
        } else {
            divident[i] = Lo_32(partial_dividend / divisor);
            remainder = Lo_32(partial_dividend - (divident[i] * divisor));
        }
    }
}

static std::string uint96_to_string(uint32_t *v)
{
    static const char *digits = "0123456789";
    std::string result;
    do {
        uint32_t rem;
        udivrem96(v, 10, rem);
        result.insert(0, 1, digits[rem]);
    } while (!uint96_is_zero(v));
    return result;
}

static HRESULT PrintDecimalValue(ICorDebugValue *pValue,
                                 std::string &output)
{
    HRESULT Status = S_OK;

    unsigned int hi;
    unsigned int mid;
    unsigned int lo;
    unsigned int flags;
    IfFailRet(GetDecimalFields(pValue, hi, mid, lo, flags));

    uint32_t v[3] = { lo, mid, hi };

    output = uint96_to_string(v);

    static const unsigned int ScaleMask = 0x00FF0000ul;
    static const unsigned int ScaleShift = 16;
    static const unsigned int SignMask = 1ul << 31;

    unsigned int scale = (flags & ScaleMask) >> ScaleShift;
    bool is_negative = flags & SignMask;

    size_t len = output.length();

    if (len > scale)
    {
        if (scale != 0) output.insert(len - scale, 1, '.');
    }
    else
    {
        output.insert(0, "0.");
        output.insert(2, scale, '0');
    }

    if (is_negative)
        output.insert(0, 1, '-');

    return S_OK;
}

static HRESULT PrintArrayValue(ICorDebugValue *pValue,
                               std::string &output)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugArrayValue> pArrayValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID*) &pArrayValue));

    ULONG32 nRank;
    IfFailRet(pArrayValue->GetRank(&nRank));
    if (nRank < 1)
    {
        return E_UNEXPECTED;
    }

    ULONG32 cElements;
    IfFailRet(pArrayValue->GetCount(&cElements));

    std::stringstream ss;
    ss << "{";

    std::string elementType;
    std::string arrayType;

    CorElementType corElemType;
    ToRelease<ICorDebugType> pFirstParameter;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    if (SUCCEEDED(pArrayValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2)) && SUCCEEDED(pValue2->GetExactType(&pType)))
    {
        if (SUCCEEDED(pType->GetFirstTypeParameter(&pFirstParameter)))
        {
            TypePrinter::GetTypeOfValue(pFirstParameter, elementType, arrayType);
        }
    }

    std::vector<ULONG32> dims(nRank, 0);
    pArrayValue->GetDimensions(nRank, &dims[0]);

    std::vector<ULONG32> base(nRank, 0);
    BOOL hasBaseIndicies = FALSE;
    if (SUCCEEDED(pArrayValue->HasBaseIndicies(&hasBaseIndicies)) && hasBaseIndicies)
        IfFailRet(pArrayValue->GetBaseIndicies(nRank, &base[0]));

    ss << elementType << "[";
    const char *sep = "";
    for (size_t i = 0; i < dims.size(); ++i)
    {
        ss << sep;
        sep = ", ";

        if (base[i] > 0)
            ss << base[i] << ".." << (base[i] + dims[i] - 1);
        else
            ss << dims[i];
    }
    ss << "]" << arrayType;

    ss << "}";
    output = ss.str();
    return S_OK;
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

    output = to_utf8(str);

    return S_OK;
}

void EscapeString(std::string &s, char q = '\"')
{
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        int count = 0;
        char c = s.at(i);
        switch (c)
        {
            case '\'':
                count = c != q ? 0 : 2;
                s.insert(i, count, '\\');
                break;
            case '\"':
                count = c != q ? 1 : 3;
                s.insert(i, count, '\\');
                break;
            case '\\':
                count = 3;
                s.insert(i, count, '\\');
                break;
            case '\0': count = 2; s.insert(i, count, '\\'); s[i + count] = '0'; break;
            case '\a': count = 2; s.insert(i, count, '\\'); s[i + count] = 'a'; break;
            case '\b': count = 2; s.insert(i, count, '\\'); s[i + count] = 'b'; break;
            case '\f': count = 2; s.insert(i, count, '\\'); s[i + count] = 'f'; break;
            case '\n': count = 2; s.insert(i, count, '\\'); s[i + count] = 'n'; break;
            case '\r': count = 2; s.insert(i, count, '\\'); s[i + count] = 'r'; break;
            case '\t': count = 2; s.insert(i, count, '\\'); s[i + count] = 't'; break;
            case '\v': count = 2; s.insert(i, count, '\\'); s[i + count] = 'v'; break;
        }
        i += count;
    }
}

HRESULT PrintValue(ICorDebugValue *pInputValue, ICorDebugILFrame * pILFrame, std::string &output)
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

        EscapeString(raw_str, '"');

        std::stringstream ss;
        ss << "\\\"" << raw_str << "\\\"";
        output = ss.str();
        return S_OK;
    }

    if (corElemType == ELEMENT_TYPE_SZARRAY || corElemType == ELEMENT_TYPE_ARRAY)
    {
        return PrintArrayValue(pValue, output);
    }

    ToRelease<ICorDebugGenericValue> pGenericValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
    IfFailRet(pGenericValue->GetValue((LPVOID) &(rgbValue[0])));

    if(IsEnum(pValue))
    {
        return PrintEnumValue(pValue, rgbValue, output);
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
            ToRelease<ICorDebugReferenceValue> pReferenceValue;
            if(SUCCEEDED(pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue)))
                pReferenceValue->GetValue(&addr);
            ss << "<function pointer 0x" << std::hex << addr << ">";
        }
        break;

    case ELEMENT_TYPE_VALUETYPE:
    case ELEMENT_TYPE_CLASS:
        {
            std::string typeName;
            TypePrinter::GetTypeOfValue(pValue, typeName);
            if (typeName == "decimal") // TODO: implement mechanism for printing custom type values
            {
                std::string val;
                PrintDecimalValue(pValue, val);
                ss << val;
            }
            else
            {
                ss << '{' << typeName << '}';
            }
        }
        break;

    case ELEMENT_TYPE_BOOLEAN:
        ss << (rgbValue[0] == 0 ? "false" : "true");
        break;

    case ELEMENT_TYPE_CHAR:
        {
            WCHAR wc = * (WCHAR *) &(rgbValue[0]);
            std::string printableVal = to_utf8(&wc, 1);
            EscapeString(printableVal, '\'');
            ss << (unsigned int)wc << " '" << printableVal << "'";
        }
        break;

    case ELEMENT_TYPE_I1:
        ss << (int) *(char*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_U1:
        ss << (unsigned int) *(unsigned char*) &(rgbValue[0]);
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
        ss << std::setprecision(8) << *(float*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_R8:
        ss << std::setprecision(16) << *(double*) &(rgbValue[0]);
        break;

    case ELEMENT_TYPE_OBJECT:
        ss << "object";
        break;

        // TODO: The following corElementTypes are not yet implemented here.  Array
        // might be interesting to add, though the others may be of rather limited use:
        //
        // ELEMENT_TYPE_GENERICINST    = 0x15,     // GENERICINST <generic type> <argCnt> <arg1> ... <argn>
    }

    output = ss.str();
    return S_OK;
}
