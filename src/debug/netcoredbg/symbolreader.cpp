// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "symbolreader.h"

#include <coreclrhost.h>
#include <thread>

#include "modules.h"
#include "platform.h"
#include "torelease.h"
#include "cputil.h"


static const char *SymbolReaderDllName = "SymbolReader";
static const char *SymbolReaderClassName = "SOS.SymbolReader";

#ifdef FEATURE_PAL
// Suppress undefined reference
// `_invalid_parameter(char16_t const*, char16_t const*, char16_t const*, unsigned int, unsigned long)':
//      /coreclr/src/pal/inc/rt/safecrt.h:386: undefined reference to `RaiseException'
static void RaiseException(DWORD dwExceptionCode,
               DWORD dwExceptionFlags,
               DWORD nNumberOfArguments,
               CONST ULONG_PTR *lpArguments)
{
}

#define ULONG_ERROR     (0xffffffffUL)
#define INTSAFE_E_ARITHMETIC_OVERFLOW       ((HRESULT)0x80070216L)  // 0x216 = 534 = ERROR_ARITHMETIC_OVERFLOW
#define UInt32x32To64(a, b) ((unsigned __int64)((ULONG)(a)) * (unsigned __int64)((ULONG)(b)))
#define WIN32_ALLOC_ALIGN (16 - 1)

__inline HRESULT ULongLongToULong(IN ULONGLONG ullOperand, OUT ULONG* pulResult)
{
    HRESULT hr = INTSAFE_E_ARITHMETIC_OVERFLOW;
    *pulResult = ULONG_ERROR;
    
    if (ullOperand <= ULONG_MAX)
    {
        *pulResult = (ULONG)ullOperand;
        hr = S_OK;
    }
    
    return hr;
}

__inline HRESULT ULongAdd(IN ULONG ulAugend, IN ULONG ulAddend, OUT ULONG* pulResult)
{
    HRESULT hr = INTSAFE_E_ARITHMETIC_OVERFLOW;
    *pulResult = ULONG_ERROR;

    if ((ulAugend + ulAddend) >= ulAugend)
    {
        *pulResult = (ulAugend + ulAddend);
        hr = S_OK;
    }
    
    return hr;
}

__inline HRESULT ULongMult(IN ULONG ulMultiplicand, IN ULONG ulMultiplier, OUT ULONG* pulResult)
{
    ULONGLONG ull64Result = UInt32x32To64(ulMultiplicand, ulMultiplier);
    
    return ULongLongToULong(ull64Result, pulResult);
}

inline HRESULT CbSysStringSize(ULONG cchSize, BOOL isByteLen, ULONG *result)
{
    if (result == NULL)
        return E_INVALIDARG;

    // +2 for the null terminator
    // + DWORD_PTR to store the byte length of the string
    int constant = sizeof(WCHAR) + sizeof(DWORD_PTR) + WIN32_ALLOC_ALIGN;

    if (isByteLen)
    {
        if (SUCCEEDED(ULongAdd(constant, cchSize, result)))
        {
            *result = *result & ~WIN32_ALLOC_ALIGN;
            return NOERROR;
        }
    }
    else
    {
        ULONG temp = 0; // should not use in-place addition in ULongAdd
        if (SUCCEEDED(ULongMult(cchSize, sizeof(WCHAR), &temp)) &
            SUCCEEDED(ULongAdd(temp, constant, result)))
        {
            *result = *result & ~WIN32_ALLOC_ALIGN;
            return NOERROR;
        }
    }
    return INTSAFE_E_ARITHMETIC_OVERFLOW;
}

BSTR PAL_SysAllocStringLen(const OLECHAR *psz, UINT len)
{
    BSTR bstr;
    DWORD cbTotal = 0;

    if (FAILED(CbSysStringSize(len, FALSE, &cbTotal)))
        return NULL;

    bstr = (OLECHAR *)malloc(cbTotal);

    if(bstr != NULL){

#if defined(_WIN64)
      // NOTE: There are some apps which peek back 4 bytes to look at the size of the BSTR. So, in case of 64-bit code,
      // we need to ensure that the BSTR length can be found by looking one DWORD before the BSTR pointer. 
      *(DWORD_PTR *)bstr = (DWORD_PTR) 0;
      bstr = (BSTR) ((char *) bstr + sizeof (DWORD));
#endif
      *(DWORD FAR*)bstr = (DWORD)len * sizeof(OLECHAR);

      bstr = (BSTR) ((char*) bstr + sizeof(DWORD));

      if(psz != NULL){
            memcpy(bstr, psz, len * sizeof(OLECHAR));
      }

      bstr[len] = '\0'; // always 0 terminate
    }

    return bstr;
}

void PAL_SysFreeString(BSTR bstr)
{
    if(bstr == NULL)
      return;
    free((BYTE *)bstr-sizeof(DWORD_PTR));    
}

unsigned int PAL_SysStringLen(BSTR bstr)
{
    if(bstr == NULL)
      return 0;
    return (unsigned int)((((DWORD FAR*)bstr)[-1]) / sizeof(OLECHAR));
}

LPVOID PAL_CoTaskMemAlloc(size_t size)
{
    return malloc(size);
}

void PAL_CoTaskMemFree(LPVOID pt)
{
    free(pt);
}

SysAllocStringLen_t SymbolReader::sysAllocStringLen = PAL_SysAllocStringLen;
SysFreeString_t SymbolReader::sysFreeString = PAL_SysFreeString;
SysStringLen_t SymbolReader::sysStringLen = PAL_SysStringLen;
CoTaskMemAlloc_t SymbolReader::coTaskMemAlloc = PAL_CoTaskMemAlloc;
CoTaskMemFree_t SymbolReader::coTaskMemFree = PAL_CoTaskMemFree;

#else

SysAllocStringLen_t SymbolReader::sysAllocStringLen = SysAllocStringLen;
SysFreeString_t SymbolReader::sysFreeString = SysFreeString;
SysStringLen_t SymbolReader::sysStringLen = SysStringLen;
CoTaskMemAlloc_t SymbolReader::coTaskMemAlloc = CoTaskMemAlloc;
CoTaskMemFree_t SymbolReader::coTaskMemFree = CoTaskMemFree;

#endif // FEATURE_PAL

std::string SymbolReader::coreClrPath;
LoadSymbolsForModuleDelegate SymbolReader::loadSymbolsForModuleDelegate;
DisposeDelegate SymbolReader::disposeDelegate;
ResolveSequencePointDelegate SymbolReader::resolveSequencePointDelegate;
GetLocalVariableNameAndScope SymbolReader::getLocalVariableNameAndScopeDelegate;
GetSequencePointByILOffsetDelegate SymbolReader::getSequencePointByILOffsetDelegate;
GetStepRangesFromIPDelegate SymbolReader::getStepRangesFromIPDelegate;
GetSequencePointsDelegate SymbolReader::getSequencePointsDelegate;
ParseExpressionDelegate SymbolReader::parseExpressionDelegate = nullptr;
EvalExpressionDelegate SymbolReader::evalExpressionDelegate = nullptr;
RegisterGetChildDelegate SymbolReader::registerGetChildDelegate = nullptr;
StringToUpperDelegate SymbolReader::stringToUpperDelegate = nullptr;

const int SymbolReader::HiddenLine = 0xfeefee;

HRESULT SymbolReader::LoadSymbols(IMetaDataImport* pMD, ICorDebugModule* pModule)
{
    HRESULT Status = S_OK;
    BOOL isDynamic = FALSE;
    BOOL isInMemory = FALSE;
    IfFailRet(pModule->IsDynamic(&isDynamic));
    IfFailRet(pModule->IsInMemory(&isInMemory));

    if (isDynamic)
    {
        // Dynamic and in memory assemblies are a special case which we will ignore for now
        return E_FAIL;
    }

    ULONG64 peAddress = 0;
    ULONG32 peSize = 0;
    IfFailRet(pModule->GetBaseAddress(&peAddress));
    IfFailRet(pModule->GetSize(&peSize));

    return LoadSymbolsForPortablePDB(
        Modules::GetModuleFileName(pModule),
        isInMemory,
        isInMemory, // isFileLayout
        peAddress,
        peSize,
        0,          // inMemoryPdbAddress
        0           // inMemoryPdbSize
    );
}

//
// Pass to managed helper code to read in-memory PEs/PDBs
// Returns the number of bytes read.
//
int ReadMemoryForSymbols(ULONG64 address, char *buffer, int cb)
{
    ULONG read;
    if (SafeReadMemory(TO_TADDR(address), (PVOID)buffer, cb, &read))
    {
        return read;
    }
    return 0;
}

HRESULT SymbolReader::LoadSymbolsForPortablePDB(
    const std::string &modulePath,
    BOOL isInMemory,
    BOOL isFileLayout,
    ULONG64 peAddress,
    ULONG64 peSize,
    ULONG64 inMemoryPdbAddress,
    ULONG64 inMemoryPdbSize)
{
    HRESULT Status = S_OK;

    if (loadSymbolsForModuleDelegate == nullptr)
    {
        IfFailRet(PrepareSymbolReader());
    }

    // The module name needs to be null for in-memory PE's.
    const WCHAR *szModuleName = nullptr;
    auto wModulePath = to_utf16(modulePath);
    if (!isInMemory && !modulePath.empty())
    {
        szModuleName = wModulePath.c_str();
    }

    m_symbolReaderHandle = loadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress,
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (m_symbolReaderHandle == 0)
    {
        return E_FAIL;
    }

    return Status;
}

struct GetChildProxy
{
    SymbolReader::GetChildCallback &m_cb;
    static BOOL GetChild(PVOID opaque, PVOID corValue, const WCHAR* name, int *typeId, PVOID *data)
    {
        std::string uft8Name = to_utf8(name);
        return static_cast<GetChildProxy*>(opaque)->m_cb(corValue, uft8Name, typeId, data);
    }
};

HRESULT SymbolReader::PrepareSymbolReader()
{
    static bool attemptedSymbolReaderPreparation = false;
    if (attemptedSymbolReaderPreparation)
    {
        // If we already tried to set up the symbol reader, we won't try again.
        return E_FAIL;
    }

    attemptedSymbolReaderPreparation = true;

    std::string clrDir = coreClrPath.substr(0, coreClrPath.rfind(DIRECTORY_SEPARATOR_STR_A));

    HRESULT Status;

    UnsetCoreCLREnv();

    void *coreclrLib = DLOpen(coreClrPath);
    if (coreclrLib == nullptr)
    {
        // TODO: Messages like this break VSCode debugger protocol. They should be reported through Protocol class.
        fprintf(stderr, "Error: Failed to load coreclr\n");
        return E_FAIL;
    }

    void *hostHandle;
    unsigned int domainId;
    coreclr_initialize_ptr initializeCoreCLR = (coreclr_initialize_ptr)DLSym(coreclrLib, "coreclr_initialize");
    if (initializeCoreCLR == nullptr)
    {
        fprintf(stderr, "Error: coreclr_initialize not found\n");
        return E_FAIL;
    }

    std::string tpaList;
    AddFilesFromDirectoryToTpaList(clrDir, tpaList);

    const char *propertyKeys[] = {
        "TRUSTED_PLATFORM_ASSEMBLIES",
        "APP_PATHS",
        "APP_NI_PATHS",
        "NATIVE_DLL_SEARCH_DIRECTORIES",
        "AppDomainCompatSwitch"};


    std::string exe = GetExeAbsPath();
    if (exe.empty())
    {
        fprintf(stderr, "GetExeAbsPath is empty\n");
        return E_FAIL;
    }

    std::size_t dirSepIndex = exe.rfind(DIRECTORY_SEPARATOR_STR_A);
    if (dirSepIndex == std::string::npos)
        return E_FAIL;

    std::string exeDir = exe.substr(0, dirSepIndex);

    const char *propertyValues[] = {// TRUSTED_PLATFORM_ASSEMBLIES
                                    tpaList.c_str(),
                                    // APP_PATHS
                                    exeDir.c_str(),
                                    // APP_NI_PATHS
                                    exeDir.c_str(),
                                    // NATIVE_DLL_SEARCH_DIRECTORIES
                                    clrDir.c_str(),
                                    // AppDomainCompatSwitch
                                    "UseLatestBehaviorWhenTFMNotSpecified"};

    Status = initializeCoreCLR(exe.c_str(), "debugger",
        sizeof(propertyKeys) / sizeof(propertyKeys[0]), propertyKeys, propertyValues, &hostHandle, &domainId);

    if (FAILED(Status))
    {
        fprintf(stderr, "Error: Fail to initialize CoreCLR %08x\n", Status);
        return Status;
    }

    coreclr_create_delegate_ptr createDelegate = (coreclr_create_delegate_ptr)DLSym(coreclrLib, "coreclr_create_delegate");
    if (createDelegate == nullptr)
    {
        fprintf(stderr, "Error: coreclr_create_delegate not found\n");
        return E_FAIL;
    }

    // TODO: If SymbolReaderDllName could not be found, we are going to see the error message.
    //       But the cleaner way is to provide error message for any failed createDelegate().
    if (FAILED(Status = createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "LoadSymbolsForModule", (void **)&loadSymbolsForModuleDelegate)))
    {
        fprintf(stderr, "Error: createDelegate failed for LoadSymbolsForModule: 0x%x\n", Status);
        return E_FAIL;
    }
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "Dispose", (void **)&disposeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "ResolveSequencePoint", (void **)&resolveSequencePointDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLocalVariableNameAndScope", (void **)&getLocalVariableNameAndScopeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetSequencePointByILOffset", (void **)&getSequencePointByILOffsetDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetSequencePoints", (void **)&getSequencePointsDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "ParseExpression", (void **)&parseExpressionDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "EvalExpression", (void **)&evalExpressionDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "RegisterGetChild", (void **)&registerGetChildDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "StringToUpper", (void **)&stringToUpperDelegate));
    if (!registerGetChildDelegate(GetChildProxy::GetChild))
        return E_FAIL;

    // Warm up Roslyn
    std::thread([](){ std::string data; std::string err; SymbolReader::ParseExpression("1", "System.Int32", data, err); }).detach();

    return S_OK;
}

HRESULT SymbolReader::ResolveSequencePoint(const char *filename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset)
{
    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(resolveSequencePointDelegate != nullptr);

        if (resolveSequencePointDelegate(m_symbolReaderHandle, to_utf16(filename).c_str(), lineNumber, pToken, pIlOffset) == FALSE)
        {
            return E_FAIL;
        }
        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetSequencePointByILOffset(
    mdMethodDef methodToken,
    ULONG64 ilOffset,
    SequencePoint *sequencePoint)
{
    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getSequencePointByILOffsetDelegate != nullptr);

        // Sequence points with startLine equal to 0xFEEFEE marker are filtered out on the managed side.
        if (getSequencePointByILOffsetDelegate(m_symbolReaderHandle, methodToken, ilOffset, sequencePoint) == FALSE)
        {
            return E_FAIL;
        }

        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetStepRangesFromIP(ULONG32 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset)
{
    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getStepRangesFromIPDelegate != nullptr);

        if (getStepRangesFromIPDelegate(m_symbolReaderHandle, ip, MethodToken, ilStartOffset, ilEndOffset) == FALSE)
        {
            return E_FAIL;
        }

        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetNamedLocalVariableAndScope(
    ICorDebugILFrame * pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    WCHAR* paramName,
    ULONG paramNameLen,
    ICorDebugValue** ppValue,
    ULONG32* pIlStart,
    ULONG32* pIlEnd)
{
    if (!m_symbolReaderHandle)
        return E_FAIL;

    _ASSERTE(getLocalVariableNameAndScopeDelegate != nullptr);

    BSTR wszParamName = sysAllocStringLen(0, mdNameLen);
    if (sysStringLen(wszParamName) == 0)
    {
        return E_OUTOFMEMORY;
    }

    if (getLocalVariableNameAndScopeDelegate(m_symbolReaderHandle, methodToken, localIndex, &wszParamName, pIlStart, pIlEnd) == FALSE)
    {
        sysFreeString(wszParamName);
        return E_FAIL;
    }

    wcscpy_s(paramName, paramNameLen, wszParamName);
    sysFreeString(wszParamName);

    if (FAILED(pILFrame->GetLocalVariable(localIndex, ppValue)) || (*ppValue == NULL))
    {
        *ppValue = NULL;
        return E_FAIL;
    }
    return S_OK;
}

HRESULT SymbolReader::GetSequencePoints(mdMethodDef methodToken, std::vector<SequencePoint> &points)
{
    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getSequencePointsDelegate != nullptr);

        SequencePoint *allocatedPoints = nullptr;
        int pointsCount = 0;

        if (getSequencePointsDelegate(m_symbolReaderHandle, methodToken, (PVOID*)&allocatedPoints, &pointsCount) == FALSE)
        {
            return E_FAIL;
        }

        points.assign(allocatedPoints, allocatedPoints + pointsCount);

        coTaskMemFree(allocatedPoints);

        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::ParseExpression(
    const std::string &expr,
    const std::string &typeName,
    std::string &data,
    std::string &errorText)
{
    PrepareSymbolReader();

    if (parseExpressionDelegate == nullptr)
        return E_FAIL;

    BSTR werrorText;
    PVOID dataPtr;
    int dataSize = 0;
    if (parseExpressionDelegate(to_utf16(expr).c_str(), to_utf16(typeName).c_str(), &dataPtr, &dataSize, &werrorText) == FALSE)
    {
        errorText = to_utf8(werrorText);
        sysFreeString(werrorText);
        return E_FAIL;
    }

    if (typeName == "System.String")
    {
        data = to_utf8((BSTR)dataPtr);
        sysFreeString((BSTR)dataPtr);
    }
    else
    {
        data.resize(dataSize);
        memmove(&data[0], dataPtr, dataSize);
        coTaskMemFree(dataPtr);
    }

    return S_OK;
}

HRESULT SymbolReader::EvalExpression(const std::string &expr, std::string &result, int *typeId, ICorDebugValue **ppValue, GetChildCallback cb)
{
    PrepareSymbolReader();

    if (evalExpressionDelegate == nullptr)
        return E_FAIL;

    GetChildProxy proxy { cb };

    PVOID valuePtr = nullptr;
    int size = 0;
    BSTR resultText;
    BOOL ok = evalExpressionDelegate(to_utf16(expr).c_str(), &proxy, &resultText, typeId, &size, &valuePtr);
    if (!ok)
    {
        if (resultText)
        {
            result = to_utf8(resultText);
            sysFreeString(resultText);
        }
        return E_FAIL;
    }

    switch(*typeId)
    {
        case TypeCorValue:
            *ppValue = static_cast<ICorDebugValue*>(valuePtr);
            if (*ppValue)
                (*ppValue)->AddRef();
            break;
        case TypeObject:
            result = std::string();
            break;
        case TypeString:
            result = to_utf8((BSTR)valuePtr);
            sysFreeString((BSTR)valuePtr);
            break;
        default:
            result.resize(size);
            memmove(&result[0], valuePtr, size);
            coTaskMemFree(valuePtr);
            break;
    }

    return S_OK;
}

PVOID SymbolReader::AllocBytes(size_t size)
{
    PrepareSymbolReader();
    return coTaskMemAlloc(size);
}

PVOID SymbolReader::AllocString(const std::string &str)
{
    PrepareSymbolReader();
    auto wstr = to_utf16(str);
    if (wstr.size() > UINT_MAX)
        return nullptr;
    BSTR bstr = sysAllocStringLen(0, (UINT)wstr.size());
    if (sysStringLen(bstr) == 0)
        return nullptr;
    memmove(bstr, wstr.data(), wstr.size() * sizeof(decltype(wstr[0])));
    return bstr;
}

HRESULT SymbolReader::StringToUpper(std::string &String)
{
    PrepareSymbolReader();

    if (stringToUpperDelegate == nullptr)
        return E_FAIL;

    BSTR wString;
    stringToUpperDelegate(to_utf16(String).c_str(), &wString);

    if (!wString)
        return E_FAIL;

    String = to_utf8(wString);
    sysFreeString(wString);

    return S_OK;
}
