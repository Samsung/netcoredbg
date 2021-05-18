// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "managed/interop.h"

#include <coreclrhost.h>
#include <thread>
#include <string>

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

enum class RetCode : int32_t
{
    OK = 0,
    Fail = 1,
    Exception = 2
};

Utility::RWLock CLRrwlock;
void *hostHandle = nullptr;
unsigned int domainId = 0;
coreclr_shutdown_ptr shutdownCoreClr = nullptr;

// CoreCLR use fixed size integers, don't use system/arch size dependant types for delegates.
// Important! In case of usage pointer to variable as delegate arg, make sure it have proper size for CoreCLR!
// For example, native code "int" != managed code "int", since managed code "int" is 4 byte fixed size.
typedef  int (*ReadMemoryDelegate)(uint64_t, char*, int32_t);
typedef  PVOID (*LoadSymbolsForModuleDelegate)(const WCHAR*, BOOL, uint64_t, int32_t, uint64_t, int32_t, ReadMemoryDelegate);
typedef  void (*DisposeDelegate)(PVOID);
typedef  RetCode (*GetLocalVariableNameAndScope)(PVOID, int32_t, int32_t, BSTR*, uint32_t*, uint32_t*);
typedef  RetCode (*GetSequencePointByILOffsetDelegate)(PVOID, mdMethodDef, uint32_t, PVOID);
typedef  RetCode (*GetNextSequencePointByILOffsetDelegate)(PVOID, mdMethodDef, uint32_t, uint32_t*, int32_t*);
typedef  RetCode (*GetStepRangesFromIPDelegate)(PVOID, int32_t, mdMethodDef, uint32_t*, uint32_t*);
typedef  RetCode (*GetModuleMethodsRangesDelegate)(PVOID, int32_t, PVOID, int32_t, PVOID, PVOID*);
typedef  RetCode (*ResolveBreakPointsDelegate)(PVOID, int32_t, PVOID, int32_t, int32_t, int32_t*, PVOID*);
typedef  RetCode (*GetMethodLastIlOffsetDelegate)(PVOID, mdMethodDef, uint32_t*);
typedef  RetCode (*GetAsyncMethodsSteppingInfoDelegate)(PVOID, PVOID*, int32_t*);
typedef  RetCode (*ParseExpressionDelegate)(const WCHAR*, const WCHAR*, PVOID*, int32_t*, BSTR*);
typedef  RetCode (*EvalExpressionDelegate)(const WCHAR*, PVOID, BSTR*, int32_t*, int32_t*, PVOID*);
typedef  BOOL (*GetChildDelegate)(PVOID, PVOID, const WCHAR*, int32_t*, PVOID*);
typedef  BOOL (*RegisterGetChildDelegate)(GetChildDelegate);
typedef  RetCode (*StringToUpperDelegate)(const WCHAR*, BSTR*);
typedef  void (*GCCollectDelegate)();
typedef  PVOID (*CoTaskMemAllocDelegate)(int32_t);
typedef  void (*CoTaskMemFreeDelegate)(PVOID);
typedef  PVOID (*SysAllocStringLenDelegate)(int32_t);
typedef  void (*SysFreeStringDelegate)(PVOID);

LoadSymbolsForModuleDelegate loadSymbolsForModuleDelegate = nullptr;
DisposeDelegate disposeDelegate = nullptr;
GetLocalVariableNameAndScope getLocalVariableNameAndScopeDelegate = nullptr;
GetSequencePointByILOffsetDelegate getSequencePointByILOffsetDelegate = nullptr;
GetNextSequencePointByILOffsetDelegate getNextSequencePointByILOffsetDelegate = nullptr;
GetStepRangesFromIPDelegate getStepRangesFromIPDelegate = nullptr;
GetModuleMethodsRangesDelegate getModuleMethodsRangesDelegate = nullptr;
ResolveBreakPointsDelegate resolveBreakPointsDelegate = nullptr;
GetMethodLastIlOffsetDelegate getMethodLastIlOffsetDelegate = nullptr;
GetAsyncMethodsSteppingInfoDelegate getAsyncMethodsSteppingInfoDelegate = nullptr;
ParseExpressionDelegate parseExpressionDelegate = nullptr;
EvalExpressionDelegate evalExpressionDelegate = nullptr;
RegisterGetChildDelegate registerGetChildDelegate = nullptr;
StringToUpperDelegate stringToUpperDelegate = nullptr;
GCCollectDelegate gCCollectDelegate = nullptr;
CoTaskMemAllocDelegate coTaskMemAllocDelegate = nullptr;
CoTaskMemFreeDelegate coTaskMemFreeDelegate = nullptr;
SysAllocStringLenDelegate sysAllocStringLenDelegate = nullptr;
SysFreeStringDelegate sysFreeStringDelegate = nullptr;

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


SequencePoint::~SequencePoint() noexcept
{
    Interop::SysFreeString(document);
}

HRESULT LoadSymbols(IMetaDataImport *pMD, ICorDebugModule *pModule, VOID **ppSymbolReaderHandle)
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
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetLocalVariableNameAndScope", (void **)&getLocalVariableNameAndScopeDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSequencePointByILOffset", (void **)&getSequencePointByILOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetNextSequencePointByILOffset", (void **)&getNextSequencePointByILOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetModuleMethodsRanges", (void **)&getModuleMethodsRangesDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "ResolveBreakPoints", (void **)&resolveBreakPointsDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetMethodLastIlOffset", (void **)&getMethodLastIlOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetAsyncMethodsSteppingInfo", (void **)&getAsyncMethodsSteppingInfoDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "ParseExpression", (void **)&parseExpressionDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "EvalExpression", (void **)&evalExpressionDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "RegisterGetChild", (void **)&registerGetChildDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "StringToUpper", (void **)&stringToUpperDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "GCCollect", (void **)&gCCollectDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "CoTaskMemAlloc", (void **)&coTaskMemAllocDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "CoTaskMemFree", (void **)&coTaskMemFreeDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "SysAllocStringLen", (void **)&sysAllocStringLenDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "SysFreeString", (void **)&sysFreeStringDelegate));

    if (!allDelegatesCreated)
        throw std::runtime_error("createDelegate failed with status: " + std::to_string(Status));

    bool allDelegatesInited = loadSymbolsForModuleDelegate &&
                              disposeDelegate &&
                              getLocalVariableNameAndScopeDelegate &&
                              getSequencePointByILOffsetDelegate &&
                              getNextSequencePointByILOffsetDelegate &&
                              getStepRangesFromIPDelegate &&
                              getModuleMethodsRangesDelegate &&
                              resolveBreakPointsDelegate &&
                              getMethodLastIlOffsetDelegate &&
                              getAsyncMethodsSteppingInfoDelegate &&
                              parseExpressionDelegate &&
                              evalExpressionDelegate &&
                              registerGetChildDelegate &&
                              stringToUpperDelegate &&
                              gCCollectDelegate &&
                              coTaskMemAllocDelegate &&
                              coTaskMemFreeDelegate &&
                              sysAllocStringLenDelegate &&
                              sysFreeStringDelegate;

    if (!allDelegatesInited)
        throw std::runtime_error("Some delegates nulled");

    if (!registerGetChildDelegate(GetChildProxy::GetChild))
        throw std::runtime_error("GetChildDelegate failed");

    // Warm up Roslyn
    std::thread( [](ParseExpressionDelegate parseExpressionDelegate, GCCollectDelegate gCCollectDelegate,
                    SysFreeStringDelegate sysFreeStringDelegate, CoTaskMemFreeDelegate coTaskMemFreeDelegate){
        BSTR werrorText;
        PVOID dataPtr;
        int dataSize = 0;
        parseExpressionDelegate(W("1"), W("System.Int32"), &dataPtr, &dataSize, &werrorText);
        // Dirty workaround, in order to prevent memory leak by Roslyn, since it create assembly that can't be unloaded each eval.
        // https://github.com/dotnet/roslyn/issues/22219
        // https://github.com/dotnet/roslyn/issues/41722
        gCCollectDelegate();
        sysFreeStringDelegate(werrorText);
        coTaskMemFreeDelegate(dataPtr);
    }, parseExpressionDelegate, gCCollectDelegate, sysFreeStringDelegate, coTaskMemFreeDelegate).detach();
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
    getLocalVariableNameAndScopeDelegate = nullptr;
    getSequencePointByILOffsetDelegate = nullptr;
    getNextSequencePointByILOffsetDelegate = nullptr;
    getStepRangesFromIPDelegate = nullptr;
    getModuleMethodsRangesDelegate = nullptr;
    resolveBreakPointsDelegate = nullptr;
    getMethodLastIlOffsetDelegate = nullptr;
    getAsyncMethodsSteppingInfoDelegate = nullptr;
    parseExpressionDelegate = nullptr;
    evalExpressionDelegate = nullptr;
    registerGetChildDelegate = nullptr;
    stringToUpperDelegate = nullptr;
    gCCollectDelegate = nullptr;
    coTaskMemAllocDelegate = nullptr;
    coTaskMemFreeDelegate = nullptr;
    sysAllocStringLenDelegate = nullptr;
    sysFreeStringDelegate = nullptr;
}

HRESULT GetSequencePointByILOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG32 ilOffset, SequencePoint *sequencePoint)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getSequencePointByILOffsetDelegate || !pSymbolReaderHandle || !sequencePoint)
        return E_FAIL;

    // Sequence points with startLine equal to 0xFEEFEE marker are filtered out on the managed side.
    RetCode retCode = getSequencePointByILOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset, sequencePoint);

    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetNextSequencePointByILOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG32 ilOffset, ULONG32 &ilCloseOffset, bool *noUserCodeFound)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getNextSequencePointByILOffsetDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    int32_t NoUserCodeFound = 0;

    // Sequence points with startLine equal to 0xFEEFEE marker are filtered out on the managed side.
    RetCode retCode = getNextSequencePointByILOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset, &ilCloseOffset, &NoUserCodeFound);

    if (noUserCodeFound)
        *noUserCodeFound = NoUserCodeFound == 1;

    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetStepRangesFromIP(PVOID pSymbolReaderHandle, ULONG32 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getStepRangesFromIPDelegate || !pSymbolReaderHandle || !ilStartOffset || !ilEndOffset)
        return E_FAIL;

    RetCode retCode = getStepRangesFromIPDelegate(pSymbolReaderHandle, ip, MethodToken, ilStartOffset, ilEndOffset);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetNamedLocalVariableAndScope(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG localIndex,
                                      WCHAR *paramName, ULONG paramNameLen, ULONG32 *pIlStart, ULONG32 *pIlEnd)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getLocalVariableNameAndScopeDelegate || !pSymbolReaderHandle || !paramName || !pIlStart || !pIlEnd)
        return E_FAIL;

    BSTR wszParamName = Interop::SysAllocStringLen(mdNameLen);
    if (InteropPlatform::SysStringLen(wszParamName) == 0)
        return E_OUTOFMEMORY;

    RetCode retCode = getLocalVariableNameAndScopeDelegate(pSymbolReaderHandle, methodToken, localIndex, &wszParamName, pIlStart, pIlEnd);
    read_lock.unlock();

    if (retCode != RetCode::OK)
    {
        Interop::SysFreeString(wszParamName);
        return E_FAIL;
    }

    wcscpy_s(paramName, paramNameLen, wszParamName);
    Interop::SysFreeString(wszParamName);

    return S_OK;
}

HRESULT GetMethodLastIlOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG32 *ilOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getMethodLastIlOffsetDelegate || !pSymbolReaderHandle || !ilOffset)
        return E_FAIL;

    RetCode retCode = getMethodLastIlOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetModuleMethodsRanges(PVOID pSymbolReaderHandle, int32_t constrTokensNum, PVOID constrTokens, int32_t normalTokensNum, PVOID normalTokens, PVOID *data)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getModuleMethodsRangesDelegate || !pSymbolReaderHandle || (constrTokensNum && !constrTokens) || (normalTokensNum && !normalTokens) || !data)
        return E_FAIL;

    RetCode retCode = getModuleMethodsRangesDelegate(pSymbolReaderHandle, constrTokensNum, constrTokens, normalTokensNum, normalTokens, data);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT ResolveBreakPoints(PVOID pSymbolReaderHandle, int32_t tokenNum, PVOID Tokens, int32_t sourceLine, int32_t nestedToken, int32_t &Count, PVOID *data)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!resolveBreakPointsDelegate || !pSymbolReaderHandle || !Tokens || !data)
        return E_FAIL;

    RetCode retCode = resolveBreakPointsDelegate(pSymbolReaderHandle, tokenNum, Tokens, sourceLine, nestedToken, &Count, data);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetAsyncMethodsSteppingInfo(PVOID pSymbolReaderHandle, std::vector<AsyncAwaitInfoBlock> &AsyncAwaitInfo)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getAsyncMethodsSteppingInfoDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    AsyncAwaitInfoBlock *allocatedAsyncInfo = nullptr;
    int32_t asyncInfoCount = 0;

    RetCode retCode = getAsyncMethodsSteppingInfoDelegate(pSymbolReaderHandle, (PVOID*)&allocatedAsyncInfo, &asyncInfoCount);
    read_lock.unlock();

    if (retCode != RetCode::OK)
        return E_FAIL;

    if (asyncInfoCount == 0)
    {
        assert(allocatedAsyncInfo == nullptr);
        return S_OK;
    }

    AsyncAwaitInfo.assign(allocatedAsyncInfo, allocatedAsyncInfo + asyncInfoCount);

    Interop::CoTaskMemFree(allocatedAsyncInfo);
    return S_OK;
}

HRESULT ParseExpression(const std::string &expr, const std::string &typeName, std::string &data, std::string &errorText)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!parseExpressionDelegate || !gCCollectDelegate)
        return E_FAIL;

    BSTR werrorText;
    PVOID dataPtr;
    int32_t dataSize = 0;
    RetCode retCode = parseExpressionDelegate(to_utf16(expr).c_str(), to_utf16(typeName).c_str(), &dataPtr, &dataSize, &werrorText);
    // Dirty workaround, in order to prevent memory leak by Roslyn, since it create assembly that can't be unloaded each eval.
    // https://github.com/dotnet/roslyn/issues/22219
    // https://github.com/dotnet/roslyn/issues/41722
    gCCollectDelegate();

    read_lock.unlock();
    
    if (retCode != RetCode::OK)
    {
        errorText = to_utf8(werrorText);
        Interop::SysFreeString(werrorText);
        return E_FAIL;
    }

    if (typeName == "System.String")
    {
        data = to_utf8((BSTR)dataPtr);
        Interop::SysFreeString((BSTR)dataPtr);
    }
    else
    {
        data.resize(dataSize);
        memmove(&data[0], dataPtr, dataSize);
        Interop::CoTaskMemFree(dataPtr);
    }

    return S_OK;
}

HRESULT EvalExpression(const std::string &expr, std::string &result, int *typeId, ICorDebugValue **ppValue, GetChildCallback cb)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!evalExpressionDelegate || !gCCollectDelegate || !typeId || !ppValue)
        return E_FAIL;

    GetChildProxy proxy { cb };
    PVOID valuePtr = nullptr;
    int32_t size = 0;
    BSTR resultText;
    RetCode retCode = evalExpressionDelegate(to_utf16(expr).c_str(), &proxy, &resultText, typeId, &size, &valuePtr);
    // Dirty workaround, in order to prevent memory leak by Roslyn, since it create assembly that can't be unloaded each eval.
    // https://github.com/dotnet/roslyn/issues/22219
    // https://github.com/dotnet/roslyn/issues/41722
    gCCollectDelegate();

    read_lock.unlock();

    if (retCode != RetCode::OK)
    {
        if (resultText)
        {
            result = to_utf8(resultText);
            Interop::SysFreeString(resultText);
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
            Interop::SysFreeString((BSTR)valuePtr);
            break;
        default:
            result.resize(size);
            memmove(&result[0], valuePtr, size);
            Interop::CoTaskMemFree(valuePtr);
            break;
    }

    return S_OK;
}

PVOID AllocString(const std::string &str)
{
    if (str.empty())
        return nullptr;

    auto wstr = to_utf16(str);
    BSTR bstr = Interop::SysAllocStringLen((int32_t)wstr.size());
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
    RetCode retCode = stringToUpperDelegate(to_utf16(String).c_str(), &wString);
    read_lock.unlock();

    if (retCode != RetCode::OK || !wString)
        return E_FAIL;

    String = to_utf8(wString);
    Interop::SysFreeString(wString);

    return S_OK;
}

BSTR SysAllocStringLen(int32_t size)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!sysAllocStringLenDelegate)
        return nullptr;

    return (BSTR)sysAllocStringLenDelegate(size);
}

void SysFreeString(BSTR ptrBSTR)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!sysFreeStringDelegate)
        return;

    sysFreeStringDelegate(ptrBSTR);
}

PVOID CoTaskMemAlloc(int32_t size)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!coTaskMemAllocDelegate)
        return nullptr;

    return coTaskMemAllocDelegate(size);
}

void CoTaskMemFree(PVOID ptr)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!coTaskMemFreeDelegate)
        return;

    coTaskMemFreeDelegate(ptr);
}

} // namespace Interop

} // namespace netcoredbg
