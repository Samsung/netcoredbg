// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "managed/interop.h"

#include <coreclrhost.h>
#include <thread>
#include <string>
#include <map>

#include "palclr.h"
#include "platform.h"
#include "metadata/modules.h"
#include "dynlibs.h"
#include "utils/utf.h"
#include "utils/rwlock.h"


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
#endif

namespace netcoredbg
{

namespace Interop
{

namespace // unnamed namespace
{

Utility::RWLock CLRrwlock;
void *hostHandle = nullptr;
unsigned int domainId = 0;
coreclr_shutdown_ptr shutdownCoreClr = nullptr;

typedef  int (*ReadMemoryDelegate)(ULONG64, char *, int);
typedef  PVOID (*LoadSymbolsForModuleDelegate)(const WCHAR*, BOOL, ULONG64, int, ULONG64, int, ReadMemoryDelegate);
typedef  void (*DisposeDelegate)(PVOID);
typedef  BOOL (*ResolveSequencePointDelegate)(PVOID, const WCHAR*, unsigned int, unsigned int*, unsigned int*);
typedef  BOOL (*GetLocalVariableNameAndScope)(PVOID, int, int, BSTR*, unsigned int*, unsigned int*);
typedef  BOOL (*GetSequencePointByILOffsetDelegate)(PVOID, mdMethodDef, ULONG64, PVOID);
typedef  BOOL (*GetStepRangesFromIPDelegate)(PVOID, int, mdMethodDef, unsigned int*, unsigned int*);
typedef  BOOL (*GetSequencePointsDelegate)(PVOID, mdMethodDef, PVOID*, int*);
typedef  BOOL (*HasSourceLocationDelegate)(PVOID, mdMethodDef);
typedef  BOOL (*GetMethodLastIlOffsetDelegate)(PVOID, mdMethodDef, unsigned int*);
typedef  void (*GetAsyncMethodsSteppingInfoDelegate)(PVOID, PVOID*, int*);
typedef  BOOL (*ParseExpressionDelegate)(const WCHAR*, const WCHAR*, PVOID*, int *, BSTR*);
typedef  BOOL (*EvalExpressionDelegate)(const WCHAR*, PVOID, BSTR*, int*, int*, PVOID*);
typedef  BOOL (*GetChildDelegate)(PVOID, PVOID, const WCHAR*, int *, PVOID*);
typedef  BOOL (*RegisterGetChildDelegate)(GetChildDelegate);
typedef  void (*StringToUpperDelegate)(const WCHAR*, BSTR*);

LoadSymbolsForModuleDelegate loadSymbolsForModuleDelegate = nullptr;
DisposeDelegate disposeDelegate = nullptr;
ResolveSequencePointDelegate resolveSequencePointDelegate = nullptr;
GetLocalVariableNameAndScope getLocalVariableNameAndScopeDelegate = nullptr;
GetSequencePointByILOffsetDelegate getSequencePointByILOffsetDelegate = nullptr;
GetStepRangesFromIPDelegate getStepRangesFromIPDelegate = nullptr;
GetSequencePointsDelegate getSequencePointsDelegate = nullptr;
HasSourceLocationDelegate hasSourceLocationDelegate = nullptr;
GetMethodLastIlOffsetDelegate getMethodLastIlOffsetDelegate = nullptr;
GetAsyncMethodsSteppingInfoDelegate getAsyncMethodsSteppingInfoDelegate = nullptr;
ParseExpressionDelegate parseExpressionDelegate = nullptr;
EvalExpressionDelegate evalExpressionDelegate = nullptr;
RegisterGetChildDelegate registerGetChildDelegate = nullptr;
StringToUpperDelegate stringToUpperDelegate = nullptr;

constexpr char ManagedPartDllName[] = "ManagedPart";
constexpr char SymbolReaderClassName[] = "NetCoreDbg.SymbolReader";
constexpr char EvaluationClassName[] = "NetCoreDbg.Evaluation";
constexpr char UtilsClassName[] = "NetCoreDbg.Utils";

// Pass to managed helper code to read in-memory PEs/PDBs
// Returns the number of bytes read.
int ReadMemoryForSymbols(ULONG64 address, char *buffer, int cb)
{
    // TODO: In-memory PDB?
    // OSPageSize() for Linux/Windows already implemented in code.
    return 0;
}

HRESULT LoadSymbolsForPortablePDB(const std::string &modulePath, BOOL isInMemory, BOOL isFileLayout, ULONG64 peAddress, ULONG64 peSize,
                                  ULONG64 inMemoryPdbAddress, ULONG64 inMemoryPdbSize, VOID **ppSymbolReaderHandle)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!loadSymbolsForModuleDelegate || !ppSymbolReaderHandle)
        return E_FAIL;

    // The module name needs to be null for in-memory PE's.
    const WCHAR *szModuleName = nullptr;
    auto wModulePath = to_utf16(modulePath);
    if (!isInMemory && !modulePath.empty())
    {
        szModuleName = wModulePath.c_str();
    }

    *ppSymbolReaderHandle = loadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress,
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (*ppSymbolReaderHandle == 0)
        return E_FAIL;

    return S_OK;
}

} // unnamed namespace


SequencePoint::~SequencePoint()
{
    InteropPlatform::SysFreeString(document);
}

HRESULT LoadSymbols(IMetaDataImport* pMD, ICorDebugModule* pModule, VOID **ppSymbolReaderHandle)
{
    HRESULT Status = S_OK;
    BOOL isDynamic = FALSE;
    BOOL isInMemory = FALSE;
    IfFailRet(pModule->IsDynamic(&isDynamic));
    IfFailRet(pModule->IsInMemory(&isInMemory));

    if (isDynamic)
        return E_FAIL; // Dynamic and in memory assemblies are a special case which we will ignore for now

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
        0,          // inMemoryPdbSize
        ppSymbolReaderHandle
    );
}

void DisposeSymbols(PVOID pSymbolReaderHandle)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!disposeDelegate || !pSymbolReaderHandle)
        return;

    disposeDelegate(pSymbolReaderHandle);
}

struct GetChildProxy
{
    GetChildCallback &m_cb;
    static BOOL GetChild(PVOID opaque, PVOID corValue, const WCHAR* name, int *typeId, PVOID *data)
    {
        std::string uft8Name = to_utf8(name);
        return static_cast<GetChildProxy*>(opaque)->m_cb(corValue, uft8Name, typeId, data);
    }
};

// WARNING! Due to CoreCLR limitations, Init() / Shutdown() sequence can be used only once during process execution.
// Note, init in case of error will throw exception, since this is fatal for debugger (CoreCLR can't be re-init).
void Init(const std::string &coreClrPath)
{
    std::unique_lock<Utility::RWLock::Writer> write_lock(CLRrwlock.writer);

    // If we have shutdownCoreClr initialized, we already initialized all managed part.
    if (shutdownCoreClr != nullptr)
        return;

    std::string clrDir = coreClrPath.substr(0, coreClrPath.rfind(DIRECTORY_SEPARATOR_STR_A));

    HRESULT Status;

    InteropPlatform::UnsetCoreCLREnv();

    // Pin the module - CoreCLR.so/dll does not support being unloaded.
    // "CoreCLR does not support reinitialization or unloading. Do not call `coreclr_initialize` again or unload the CoreCLR library."
    // https://docs.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting
    DLHandle coreclrLib = DLOpen(coreClrPath);
    if (coreclrLib == nullptr)
        throw std::invalid_argument("Failed to load coreclr path=" + coreClrPath);

    coreclr_initialize_ptr initializeCoreCLR = (coreclr_initialize_ptr)DLSym(coreclrLib, "coreclr_initialize");
    if (initializeCoreCLR == nullptr)
        throw std::invalid_argument("coreclr_initialize not found in lib, CoreCLR path=" + coreClrPath);

    std::string tpaList;
    InteropPlatform::AddFilesFromDirectoryToTpaList(clrDir, tpaList);

    const char *propertyKeys[] = {
        "TRUSTED_PLATFORM_ASSEMBLIES",
        "APP_PATHS",
        "APP_NI_PATHS",
        "NATIVE_DLL_SEARCH_DIRECTORIES",
        "AppDomainCompatSwitch"};

    std::string exe = GetExeAbsPath();
    if (exe.empty())
        throw std::runtime_error("Unable to detect exe path");

    std::size_t dirSepIndex = exe.rfind(DIRECTORY_SEPARATOR_STR_A);
    if (dirSepIndex == std::string::npos)
        throw std::runtime_error("Can't find directory separator in string returned by GetExeAbsPath");

    std::string exeDir = exe.substr(0, dirSepIndex);

    const char *propertyValues[] = {tpaList.c_str(), // TRUSTED_PLATFORM_ASSEMBLIES
                                    exeDir.c_str(), // APP_PATHS
                                    exeDir.c_str(), // APP_NI_PATHS
                                    clrDir.c_str(), // NATIVE_DLL_SEARCH_DIRECTORIES
                                    "UseLatestBehaviorWhenTFMNotSpecified"};  // AppDomainCompatSwitch

    Status = initializeCoreCLR(exe.c_str(), "debugger",
        sizeof(propertyKeys) / sizeof(propertyKeys[0]), propertyKeys, propertyValues, &hostHandle, &domainId);

    if (FAILED(Status))
        throw std::runtime_error("Fail to initialize CoreCLR " + std::to_string(Status));

    coreclr_create_delegate_ptr createDelegate = (coreclr_create_delegate_ptr)DLSym(coreclrLib, "coreclr_create_delegate");
    if (createDelegate == nullptr)
        throw std::runtime_error("coreclr_create_delegate not found");

    shutdownCoreClr = (coreclr_shutdown_ptr)DLSym(coreclrLib, "coreclr_shutdown");
    if (shutdownCoreClr == nullptr)
        throw std::runtime_error("coreclr_shutdown not found");

    bool allDelegatesCreated = 
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "LoadSymbolsForModule", (void **)&loadSymbolsForModuleDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "Dispose", (void **)&disposeDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "ResolveSequencePoint", (void **)&resolveSequencePointDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetLocalVariableNameAndScope", (void **)&getLocalVariableNameAndScopeDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSequencePointByILOffset", (void **)&getSequencePointByILOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSequencePoints", (void **)&getSequencePointsDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "HasSourceLocation", (void **)&hasSourceLocationDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetMethodLastIlOffset", (void **)&getMethodLastIlOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetAsyncMethodsSteppingInfo", (void **)&getAsyncMethodsSteppingInfoDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "ParseExpression", (void **)&parseExpressionDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "EvalExpression", (void **)&evalExpressionDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "RegisterGetChild", (void **)&registerGetChildDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "StringToUpper", (void **)&stringToUpperDelegate));

    if (!allDelegatesCreated)
        throw std::runtime_error("createDelegate failed with status: " + std::to_string(Status));

    bool allDelegatesInited = loadSymbolsForModuleDelegate &&
                              disposeDelegate &&
                              resolveSequencePointDelegate &&
                              getLocalVariableNameAndScopeDelegate &&
                              getSequencePointByILOffsetDelegate &&
                              getStepRangesFromIPDelegate &&
                              getSequencePointsDelegate &&
                              hasSourceLocationDelegate &&
                              getMethodLastIlOffsetDelegate &&
                              getAsyncMethodsSteppingInfoDelegate &&
                              parseExpressionDelegate &&
                              evalExpressionDelegate &&
                              registerGetChildDelegate &&
                              stringToUpperDelegate;

    if (!allDelegatesInited)
        throw std::runtime_error("Some delegates nulled");

    if (!registerGetChildDelegate(GetChildProxy::GetChild))
        throw std::runtime_error("GetChildDelegate failed");

    // Warm up Roslyn
    std::thread( [](ParseExpressionDelegate Delegate){
        BSTR werrorText;
        PVOID dataPtr;
        int dataSize = 0;
        Delegate(W("1"), W("System.Int32"), &dataPtr, &dataSize, &werrorText);
        InteropPlatform::SysFreeString(werrorText);
        InteropPlatform::CoTaskMemFree(dataPtr);
    }, parseExpressionDelegate).detach();
}

// WARNING! Due to CoreCLR limitations, Shutdown() can't be called out of the Main() scope, for example, from global object destructor.
void Shutdown()
{
    std::unique_lock<Utility::RWLock::Writer> write_lock(CLRrwlock.writer);
    if (shutdownCoreClr == nullptr)
        return;

    // "Warm up Roslyn" thread still could be running at this point, let `coreclr_shutdown` care about this.
    HRESULT Status;
    if (FAILED(Status = shutdownCoreClr(hostHandle, domainId)))
        LOGE("coreclr_shutdown failed - status: 0x%08x", Status);

    shutdownCoreClr = nullptr;
    loadSymbolsForModuleDelegate = nullptr;
    disposeDelegate = nullptr;
    resolveSequencePointDelegate = nullptr;
    getLocalVariableNameAndScopeDelegate = nullptr;
    getSequencePointByILOffsetDelegate = nullptr;
    getStepRangesFromIPDelegate = nullptr;
    getSequencePointsDelegate = nullptr;
    hasSourceLocationDelegate = nullptr;
    getMethodLastIlOffsetDelegate = nullptr;
    getAsyncMethodsSteppingInfoDelegate = nullptr;
    parseExpressionDelegate = nullptr;
    evalExpressionDelegate = nullptr;
    registerGetChildDelegate = nullptr;
    stringToUpperDelegate = nullptr;
}

HRESULT ResolveSequencePoint(PVOID pSymbolReaderHandle, const char *filename, ULONG32 lineNumber, mdMethodDef* pToken, ULONG32* pIlOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!resolveSequencePointDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    if (resolveSequencePointDelegate(pSymbolReaderHandle, to_utf16({filename}).c_str(), lineNumber, pToken, pIlOffset) == FALSE)
        return E_FAIL;

    return S_OK;
}

HRESULT GetSequencePointByILOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG64 ilOffset, SequencePoint *sequencePoint)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getSequencePointByILOffsetDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    // Sequence points with startLine equal to 0xFEEFEE marker are filtered out on the managed side.
    if (getSequencePointByILOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset, sequencePoint) == FALSE)
        return E_FAIL;

    return S_OK;
}

HRESULT GetStepRangesFromIP(PVOID pSymbolReaderHandle, ULONG32 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getStepRangesFromIPDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    if (getStepRangesFromIPDelegate(pSymbolReaderHandle, ip, MethodToken, ilStartOffset, ilEndOffset) == FALSE)
        return E_FAIL;

    return S_OK;
}

HRESULT GetNamedLocalVariableAndScope(PVOID pSymbolReaderHandle, ICorDebugILFrame * pILFrame, mdMethodDef methodToken, ULONG localIndex,
                                                   WCHAR* paramName, ULONG paramNameLen, ICorDebugValue** ppValue, ULONG32* pIlStart, ULONG32* pIlEnd)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getLocalVariableNameAndScopeDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    BSTR wszParamName = InteropPlatform::SysAllocStringLen(0, mdNameLen);
    if (InteropPlatform::SysStringLen(wszParamName) == 0)
        return E_OUTOFMEMORY;

    BOOL ok = getLocalVariableNameAndScopeDelegate(pSymbolReaderHandle, methodToken, localIndex, &wszParamName, pIlStart, pIlEnd);
    read_lock.unlock();

    if (!ok)
    {
        InteropPlatform::SysFreeString(wszParamName);
        return E_FAIL;
    }

    wcscpy_s(paramName, paramNameLen, wszParamName);
    InteropPlatform::SysFreeString(wszParamName);

    if (FAILED(pILFrame->GetLocalVariable(localIndex, ppValue)) || (*ppValue == NULL))
    {
        *ppValue = NULL;
        return E_FAIL;
    }
    return S_OK;
}

HRESULT GetSequencePoints(PVOID pSymbolReaderHandle, mdMethodDef methodToken, std::vector<SequencePoint> &points)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getSequencePointsDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    SequencePoint *allocatedPoints = nullptr;
    int pointsCount = 0;

    BOOL ok = getSequencePointsDelegate(pSymbolReaderHandle, methodToken, (PVOID*)&allocatedPoints, &pointsCount);
    read_lock.unlock();

    if (!ok)
        return E_FAIL;

    points.resize(pointsCount);
    std::move(allocatedPoints, allocatedPoints + pointsCount, points.begin());

    InteropPlatform::CoTaskMemFree(allocatedPoints);
    return S_OK;
}

bool HasSourceLocation(PVOID pSymbolReaderHandle, mdMethodDef methodToken)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!hasSourceLocationDelegate || !pSymbolReaderHandle)
        return false;

    BOOL ok = hasSourceLocationDelegate(pSymbolReaderHandle, methodToken);
    return !!(ok);
}

HRESULT GetMethodLastIlOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG32 *ilOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getMethodLastIlOffsetDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    BOOL ok = getMethodLastIlOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset);
    return ok ? S_OK : E_FAIL;
}

HRESULT GetAsyncMethodsSteppingInfo(PVOID pSymbolReaderHandle, std::vector<AsyncAwaitInfoBlock> &AsyncAwaitInfo)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getAsyncMethodsSteppingInfoDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    AsyncAwaitInfoBlock *allocatedAsyncInfo = nullptr;
    int asyncInfoCount = 0;

    getAsyncMethodsSteppingInfoDelegate(pSymbolReaderHandle, (PVOID*)&allocatedAsyncInfo, &asyncInfoCount);
    read_lock.unlock();

    if (asyncInfoCount == 0)
    {
        assert(allocatedAsyncInfo == nullptr);
        return S_OK;
    }

    AsyncAwaitInfo.assign(allocatedAsyncInfo, allocatedAsyncInfo + asyncInfoCount);

    InteropPlatform::CoTaskMemFree(allocatedAsyncInfo);
    return S_OK;
}

HRESULT ParseExpression(const std::string &expr, const std::string &typeName, std::string &data, std::string &errorText)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!parseExpressionDelegate)
        return E_FAIL;

    BSTR werrorText;
    PVOID dataPtr;
    int dataSize = 0;
    BOOL ok = parseExpressionDelegate(to_utf16(expr).c_str(), to_utf16(typeName).c_str(), &dataPtr, &dataSize, &werrorText);
    read_lock.unlock();
    
    if (!ok)
    {
        errorText = to_utf8(werrorText);
        InteropPlatform::SysFreeString(werrorText);
        return E_FAIL;
    }

    if (typeName == "System.String")
    {
        data = to_utf8((BSTR)dataPtr);
        InteropPlatform::SysFreeString((BSTR)dataPtr);
    }
    else
    {
        data.resize(dataSize);
        memmove(&data[0], dataPtr, dataSize);
        InteropPlatform::CoTaskMemFree(dataPtr);
    }

    return S_OK;
}

HRESULT EvalExpression(const std::string &expr, std::string &result, int *typeId, ICorDebugValue **ppValue, GetChildCallback cb)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!evalExpressionDelegate)
        return E_FAIL;

    GetChildProxy proxy { cb };
    PVOID valuePtr = nullptr;
    int size = 0;
    BSTR resultText;
    BOOL ok = evalExpressionDelegate(to_utf16(expr).c_str(), &proxy, &resultText, typeId, &size, &valuePtr);
    read_lock.unlock();

    if (!ok)
    {
        if (resultText)
        {
            result = to_utf8(resultText);
            InteropPlatform::SysFreeString(resultText);
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
            InteropPlatform::SysFreeString((BSTR)valuePtr);
            break;
        default:
            result.resize(size);
            memmove(&result[0], valuePtr, size);
            InteropPlatform::CoTaskMemFree(valuePtr);
            break;
    }

    return S_OK;
}

PVOID AllocBytes(size_t size)
{
    return InteropPlatform::CoTaskMemAlloc(size);
}

PVOID AllocString(const std::string &str)
{
    if (str.empty())
        return nullptr;

    auto wstr = to_utf16(str);
    BSTR bstr = InteropPlatform::SysAllocStringLen(0, (UINT)wstr.size());
    if (InteropPlatform::SysStringLen(bstr) == 0)
        return nullptr;

    memmove(bstr, wstr.data(), wstr.size() * sizeof(decltype(wstr[0])));
    return bstr;
}

HRESULT StringToUpper(std::string &String)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!stringToUpperDelegate)
        return E_FAIL;

    BSTR wString;
    stringToUpperDelegate(to_utf16(String).c_str(), &wString);
    read_lock.unlock();

    if (!wString)
        return E_FAIL;

    String = to_utf8(wString);
    InteropPlatform::SysFreeString(wString);

    return S_OK;
}

} // namespace Interop

} // namespace netcoredbg
