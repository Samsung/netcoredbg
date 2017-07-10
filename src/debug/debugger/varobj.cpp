#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <unordered_set>
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

#include "typeprinter.h"

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
            ToRelease<ICorDebugReferenceValue> pReferenceValue;
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

static HRESULT GetNumChild(ICorDebugValue *pInputValue,
                           ICorDebugType *pTypeCast,
                           ULONG &numstatic,
                           ULONG &numinstance)
{
    numstatic = 0;
    numinstance = 0;

    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull) return S_OK;

    ToRelease<ICorDebugArrayValue> pArrayValue;
    if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayValue)))
    {
        ULONG32 nRank;
        IfFailRet(pArrayValue->GetRank(&nRank));

        ULONG32 cElements;
        IfFailRet(pArrayValue->GetCount(&cElements));

        numinstance = cElements;

        return S_OK;
    }

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    if(pTypeCast == NULL)
        IfFailRet(pValue2->GetExactType(&pType));
    else
    {
        pType = pTypeCast;
        pType->AddRef();
    }

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
        return S_OK;

    IfFailRet(pType->GetClass(&pClass));
    IfFailRet(pClass->GetModule(&pModule));
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if(baseTypeName != "System.Object"  && baseTypeName != "System.ValueType")
        {
            // Add fields of base class
            ULONG numstaticBase = 0;
            ULONG numinstanceBase = 0;
            IfFailRet(GetNumChild(pInputValue, pBaseType, numstaticBase, numinstanceBase));
            numstatic += numstaticBase;
            numinstance += numinstanceBase;
        }
    }

    ULONG numFields = 0;
    HCORENUM fEnum = NULL;
    mdFieldDef fieldDef;
    ULONG numstaticBack = 0;
    ULONG numinstanceBack = 0;
    while(SUCCEEDED(pMD->EnumFields(&fEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        if(SUCCEEDED(pMD->GetFieldProps(fieldDef, NULL, mdName, mdNameLen, &nameLen, &fieldAttr, NULL, NULL, NULL, NULL, NULL)))
        {
            bool is_back = (char)mdName[0] == '<';
            if(fieldAttr & fdLiteral)
                continue;

            if (fieldAttr & fdStatic)
            {
                numstatic++;
                numstaticBack += is_back ? 1 : 0;
            }
            else
            {
                numinstance++;
                numinstanceBack += is_back ? 1 : 0;
            }
        }
    }
    pMD->CloseEnum(fEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, currentTypeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdTypeDef  propertyClass;

        DWORD propFlags;
        UVCP_CONSTANT pDefaultValue;
        ULONG cchDefaultValue;
        mdMethodDef mdGetter;
        mdMethodDef rmdOtherMethod;
        ULONG cOtherMethod;
        WCHAR propertyName[mdNameLen] = W("\0");
        ULONG propertyNameLen = 0;
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef,
                                            &propertyClass,
                                            propertyName,
                                            mdNameLen,
                                            &propertyNameLen,
                                            &propFlags,
                                            NULL,
                                            NULL,
                                            NULL,
                                            &pDefaultValue,
                                            &cchDefaultValue,
                                            NULL,
                                            &mdGetter,
                                            NULL,
                                            0,
                                            NULL)))
        {
            DWORD getterAttr = 0;
            if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
                continue;

            if (getterAttr & mdStatic)
            {
                if (numstaticBack == 0)
                    numstatic++;
                else
                    numstaticBack--;
            }
            else
            {
                if (numinstanceBack == 0)
                    numinstance++;
                else
                    numinstanceBack--;
            }
        }
    }
    pMD->CloseEnum(propEnum);

    return S_OK;
}

HRESULT GetNumChild(ICorDebugValue *pValue,
                    unsigned int &numchild,
                    bool static_members = false)
{
    HRESULT Status = S_OK;
    numchild = 0;

    ULONG numstatic;
    ULONG numinstance;
    IfFailRet(GetNumChild(pValue, NULL, numstatic, numinstance));
    if (static_members)
    {
        numchild = numstatic;
    }
    else
    {
        numchild = (numstatic > 0) ? numinstance + 1 : numinstance;
    }
    return S_OK;
}

extern std::mutex g_currentThreadMutex;
extern ICorDebugThread *g_currentThread;

std::mutex g_evalMutex;
std::condition_variable g_evalCV;
bool g_evalComplete = false;

void NotifyEvalComplete()
{
    std::lock_guard<std::mutex> lock(g_evalMutex);
    g_evalComplete = true;
    g_evalMutex.unlock();
    g_evalCV.notify_one();
}

HRESULT EvalProperty(
    mdMethodDef methodDef,
    ICorDebugModule *pModule,
    ICorDebugType *pType,
    ICorDebugValue *pInputValue,
    bool is_static,
    ICorDebugValue **ppEvalResult)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugEval> pEval;

    ToRelease<ICorDebugProcess> pProcess;
    {
        // g_currentThreadMutex should be locked by caller function
        //std::lock_guard<std::mutex> lock(g_currentThreadMutex);

        IfFailRet(g_currentThread->GetProcess(&pProcess));
        IfFailRet(g_currentThread->CreateEval(&pEval));
    }

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(methodDef, &pFunc));

    ToRelease<ICorDebugTypeEnum> pTypeEnum;

    std::vector< ToRelease<ICorDebugType> > typeParams;
    if(SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ICorDebugType *curType;
        ULONG fetched = 0;
        while(SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
        {
            typeParams.emplace_back(curType);
        }
    }

    ToRelease<ICorDebugEval2> pEval2;
    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));

    IfFailRet(pEval2->CallParameterizedFunction(
        pFunc,
        typeParams.size(),
        (ICorDebugType **)typeParams.data(),
        is_static ? 0 : 1,
        is_static ? NULL : &pInputValue
    ));

    std::unique_lock<std::mutex> lock(g_evalMutex);
    g_evalComplete = false;
    IfFailRet(pProcess->Continue(0));
    g_evalCV.wait(lock, []{return g_evalComplete;});

    return pEval->GetResult(ppEvalResult);
}

struct VarObjValue
{
    std::string name;
    ICorDebugValue *value;
    std::string owningType;
    std::string typeName;

    std::string varobjName;
    bool statics_only;

    unsigned int numchild;

    VarObjValue(
        const std::string &n,
        ICorDebugValue *v,
        const std::string t = "") : name(n), value(v), owningType(t),
                                statics_only(false), numchild(0) {}
    VarObjValue(
        ICorDebugValue *v) : name("Static members"), value(v),
                                statics_only(true), numchild(0) {}
};

static HRESULT PrintFieldsAndProperties(ICorDebugValue *pInputValue,
                                        ICorDebugType *pTypeCast,
                                        ICorDebugILFrame *pILFrame,
                                        std::vector<VarObjValue> &members,
                                        bool static_members,
                                        bool &has_static_members)
{
    has_static_members = false;
    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull) return S_OK;

    ToRelease<ICorDebugArrayValue> pArrayValue;
    if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayValue)))
    {
        ULONG32 nRank;
        IfFailRet(pArrayValue->GetRank(&nRank));

        // TODO: array elements

        return S_OK;
    }

    mdTypeDef currentTypeDef;
    ToRelease<ICorDebugClass> pClass;
    ToRelease<ICorDebugValue2> pValue2;
    ToRelease<ICorDebugType> pType;
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    if(pTypeCast == NULL)
        IfFailRet(pValue2->GetExactType(&pType));
    else
    {
        pType = pTypeCast;
        pType->AddRef();
    }

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
        return S_OK;

    IfFailRet(pType->GetClass(&pClass));
    IfFailRet(pClass->GetModule(&pModule));
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if(baseTypeName != "System.Object"  && baseTypeName != "System.ValueType")
        {
            // Add fields of base class
            PrintFieldsAndProperties(pInputValue, pBaseType, pILFrame, members, static_members, has_static_members);
        }
    }

    std::string className;
    TypePrinter::GetTypeOfValue(pType, className);

    std::unordered_set<std::string> backedProperies;

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
            char cName[mdNameLen] = {0};
            WideCharToMultiByte(CP_UTF8, 0, mdName, (int)(nameLen + 1), cName, _countof(cName), NULL, NULL);

            if(fieldAttr & fdLiteral)
                continue;

            bool is_static = (fieldAttr & fdStatic);
            if (is_static)
                has_static_members = true;

            bool add_member = static_members ? is_static : !is_static;
            if (!add_member)
                continue;

            ToRelease<ICorDebugValue> pFieldVal;

            if (fieldAttr & fdStatic)
            {
                pType->GetStaticFieldValue(fieldDef, pILFrame, &pFieldVal);
            }
            else
            {
                ToRelease<ICorDebugObjectValue> pObjValue;
                if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue)))
                    pObjValue->GetFieldValue(pClass, fieldDef, &pFieldVal);
            }

            if(pFieldVal != NULL)
            {
                std::string name(cName);
                if (cName[0] == '<')
                {
                    size_t endOffset = name.rfind('>');
                    name = name.substr(1, endOffset - 1);
                    backedProperies.insert(name);
                }

                members.emplace_back(name, pFieldVal.Detach(), className);
            }
            else
            {
                // no need for backing field when we can not get its value
                if (cName[0] == '<')
                    continue;

                members.emplace_back(cName, nullptr, className);
            }
        }
    }
    pMD->CloseEnum(fEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, currentTypeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdTypeDef  propertyClass;

        ULONG propertyNameLen = 0;
        DWORD propFlags;
        PCCOR_SIGNATURE pvSig;
        ULONG pbSig;
        DWORD dwCPlusTypeFlag;
        UVCP_CONSTANT pDefaultValue;
        ULONG cchDefaultValue;
        mdMethodDef mdSetter;
        mdMethodDef mdGetter;
        mdMethodDef rmdOtherMethod;
        ULONG cOtherMethod;
        WCHAR propertyName[mdNameLen] = W("\0");
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef,
                                            &propertyClass,
                                            propertyName,
                                            mdNameLen,
                                            &propertyNameLen,
                                            &propFlags,
                                            &pvSig,
                                            &pbSig,
                                            &dwCPlusTypeFlag,
                                            &pDefaultValue,
                                            &cchDefaultValue,
                                            &mdSetter,
                                            &mdGetter,
                                            &rmdOtherMethod,
                                            1,
                                            &cOtherMethod)))
        {
            DWORD getterAttr = 0;
            if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
                continue;

            char cName[mdNameLen] = {0};
            WideCharToMultiByte(CP_UTF8, 0, propertyName, (int)(propertyNameLen + 1), cName, _countof(cName), NULL, NULL);

            if (backedProperies.find(cName) != backedProperies.end())
                continue;

            bool is_static = (getterAttr & mdStatic);
            if (is_static)
                has_static_members = true;

            bool add_member = static_members ? is_static : !is_static;
            if (!add_member)
                continue;

            ToRelease<ICorDebugValue> pResultValue;
            std::string resultTypeName;
            if (SUCCEEDED(EvalProperty(mdGetter, pModule, pType, pInputValue, is_static, &pResultValue)))
            {
                members.emplace_back(cName, pResultValue.Detach(), className);
            }
        }
    }
    pMD->CloseEnum(propEnum);

    return S_OK;
}

void FixupInheritedFieldNames(std::vector<VarObjValue> &members)
{
    std::unordered_set<std::string> names;
    for (auto it = members.rbegin(); it != members.rend(); ++it)
    {
        auto r = names.insert(it->name);
        if (!r.second)
        {
            it->name += " (" + it->owningType + ")";
        }
    }
}

void PrintChildren(std::vector<VarObjValue> &members, std::string &output)
{
    std::stringstream ss;
    ss << "numchild=\"" << members.size() << "\"";

    if (members.empty())
    {
        output = ss.str();
        return;
    }
    ss << ",children=[";

    const char *sep = "";
    for (auto m : members)
    {
        ss << sep;
        sep = ",";

        ss << "child={name=\"" << m.varobjName << "\",exp=\"" << m.name << "\",";
        ss << "numchild=\"" << m.numchild << "\",type=\"" << m.typeName << "\"}";
        //thread-id="452958",has_more="0"}
    }

    ss << "]";
    output = ss.str();
}

HRESULT ListChildren(VarObjValue &objValue, ICorDebugFrame *pFrame, std::string &output)
{
    HRESULT Status;

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    std::vector<VarObjValue> members;

    bool has_static_members;

    IfFailRet(PrintFieldsAndProperties(objValue.value,
                                    NULL,
                                    pILFrame,
                                    members,
                                    objValue.statics_only,
                                    has_static_members));

    if (!objValue.statics_only && has_static_members)
    {
        objValue.value->AddRef();
        members.emplace_back(objValue.value);
    }

    FixupInheritedFieldNames(members);

    for (auto &m : members)
    {
        std::string className;

        if (!m.value)
            continue;

        Status = GetNumChild(m.value, m.numchild, m.statics_only);
        TypePrinter::GetTypeOfValue(m.value, m.typeName);
    }

    PrintChildren(members, output);

    for (auto m : members)
    {
        if (m.value)
            m.value->Release();
    }

    return S_OK;
}

HRESULT ListChildren(ICorDebugValue *pInputValue, ICorDebugFrame *pFrame, std::string &output)
{
    VarObjValue val("?", pInputValue, "");
    return ListChildren(val, pFrame, output);
}

HRESULT WalkStackVars(ICorDebugFrame *pFrame,
                      std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> cb)
{
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

            IfFailRet(cb(pILFrame, pValue, cParamName));
        }
    }

    ULONG cLocals = 0;
    ToRelease<ICorDebugValueEnum> pLocalsEnum;

    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));
    IfFailRet(pLocalsEnum->GetCount(&cLocals));
    if (cLocals > 0)
    {
        for (ULONG i = 0; i < cLocals; i++)
        {
            std::string paramName;

            ToRelease<ICorDebugValue> pValue;
            Status = GetFrameNamedLocalVariable(pModule, pILFrame, methodDef, i, paramName, &pValue);

            if (FAILED(Status))
                continue;

            if (Status == S_FALSE)
                break;

            IfFailRet(cb(pILFrame, pValue, paramName));
        }
    }

    return S_OK;
}

HRESULT ListVariables(ICorDebugFrame *pFrame, std::string &output)
{
    bool printValues = true;
    bool printTypes = true;

    HRESULT Status;

    std::stringstream ss;
    ss << "variables=[";
    const char *sep = "";

    IfFailRet(WalkStackVars(pFrame, [&](ICorDebugILFrame *pILFrame, ICorDebugValue *pValue, const std::string &name) -> HRESULT
    {
        ss << sep << "{name=\"" << name << "\"";
        if (printValues)
        {
            std::string strVal;
            if (SUCCEEDED(PrintValue(pValue, pILFrame, strVal)))
                ss << ",value=\"" << strVal << "\"";
        }
        if (printTypes)
        {
            std::string strVal;
            if (SUCCEEDED(TypePrinter::GetTypeOfValue(pValue, strVal)))
                ss << ",type=\"" << strVal << "\"";
        }

        std::string test;
        ListChildren(pValue, pFrame, test);
        ss << test;

        ss << "}";
        sep = ",";
        return S_OK;
    }));

    ss << "]";
    output = ss.str();
    return S_OK;
}
