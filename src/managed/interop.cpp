// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "managed/interop.h"

#include <coreclrhost.h>
#include <thread>
#include <string>

#include "palclr.h"
#include "utils/platform.h"
#include "metadata/modules.h"
#include "utils/dynlibs.h"
#include "utils/utf.h"
#include "utils/rwlock.h"
#include "utils/filesystem.h"


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

// CoreCLR use fixed size integers, don't use system/arch size dependent types for delegates.
// Important! In case of usage pointer to variable as delegate arg, make sure it have proper size for CoreCLR!
// For example, native code "int" != managed code "int", since managed code "int" is 4 byte fixed size.
typedef  int (*ReadMemoryDelegate)(uint64_t, char*, int32_t);
typedef  PVOID (*LoadSymbolsForModuleDelegate)(const WCHAR*, BOOL, uint64_t, int32_t, uint64_t, int32_t, ReadMemoryDelegate);
typedef  void (*DisposeDelegate)(PVOID);
typedef  RetCode (*GetLocalVariableNameAndScope)(PVOID, int32_t, int32_t, BSTR*, uint32_t*, uint32_t*);
typedef  RetCode (*GetHoistedLocalScopes)(PVOID, int32_t, PVOID*, int32_t*);
typedef  RetCode (*GetSequencePointByILOffsetDelegate)(PVOID, mdMethodDef, uint32_t, PVOID);
typedef  RetCode (*GetSequencePointsDelegate)(PVOID, mdMethodDef, PVOID*, int32_t*);
typedef  RetCode (*GetNextUserCodeILOffsetDelegate)(PVOID, mdMethodDef, uint32_t, uint32_t*, int32_t*);
typedef  RetCode (*GetStepRangesFromIPDelegate)(PVOID, int32_t, mdMethodDef, uint32_t*, uint32_t*);
typedef  RetCode (*GetModuleMethodsRangesDelegate)(PVOID, uint32_t, PVOID, uint32_t, PVOID, PVOID*);
typedef  RetCode (*ResolveBreakPointsDelegate)(PVOID[], int32_t, PVOID, int32_t, int32_t, int32_t*, const WCHAR*, PVOID*);
typedef  RetCode (*GetAsyncMethodSteppingInfoDelegate)(PVOID, mdMethodDef, PVOID*, int32_t*, uint32_t*);
typedef  RetCode (*GetSourceDelegate)(PVOID, const WCHAR*, int32_t*, PVOID*);
typedef  PVOID (*LoadDeltaPdbDelegate)(const WCHAR*, PVOID*, int32_t*);
typedef  RetCode (*CalculationDelegate)(PVOID, int32_t, PVOID, int32_t, int32_t, int32_t*, PVOID*, BSTR*);
typedef  int (*GenerateStackMachineProgramDelegate)(const WCHAR*, PVOID*, BSTR*);
typedef  void (*ReleaseStackMachineProgramDelegate)(PVOID);
typedef  int (*NextStackCommandDelegate)(PVOID, int32_t*, PVOID*, BSTR*);
typedef  RetCode (*StringToUpperDelegate)(const WCHAR*, BSTR*);
typedef  PVOID (*CoTaskMemAllocDelegate)(int32_t);
typedef  void (*CoTaskMemFreeDelegate)(PVOID);
typedef  PVOID (*SysAllocStringLenDelegate)(int32_t);
typedef  void (*SysFreeStringDelegate)(PVOID);

LoadSymbolsForModuleDelegate loadSymbolsForModuleDelegate = nullptr;
DisposeDelegate disposeDelegate = nullptr;
GetLocalVariableNameAndScope getLocalVariableNameAndScopeDelegate = nullptr;
GetHoistedLocalScopes getHoistedLocalScopesDelegate = nullptr;
GetSequencePointByILOffsetDelegate getSequencePointByILOffsetDelegate = nullptr;
GetSequencePointsDelegate getSequencePointsDelegate = nullptr;
GetNextUserCodeILOffsetDelegate getNextUserCodeILOffsetDelegate = nullptr;
GetStepRangesFromIPDelegate getStepRangesFromIPDelegate = nullptr;
GetModuleMethodsRangesDelegate getModuleMethodsRangesDelegate = nullptr;
ResolveBreakPointsDelegate resolveBreakPointsDelegate = nullptr;
GetAsyncMethodSteppingInfoDelegate getAsyncMethodSteppingInfoDelegate = nullptr;
GetSourceDelegate getSourceDelegate = nullptr;
LoadDeltaPdbDelegate loadDeltaPdbDelegate = nullptr;
GenerateStackMachineProgramDelegate generateStackMachineProgramDelegate = nullptr;
ReleaseStackMachineProgramDelegate releaseStackMachineProgramDelegate = nullptr;
NextStackCommandDelegate nextStackCommandDelegate = nullptr;
StringToUpperDelegate stringToUpperDelegate = nullptr;
CoTaskMemAllocDelegate coTaskMemAllocDelegate = nullptr;
CoTaskMemFreeDelegate coTaskMemFreeDelegate = nullptr;
SysAllocStringLenDelegate sysAllocStringLenDelegate = nullptr;
SysFreeStringDelegate sysFreeStringDelegate = nullptr;
CalculationDelegate calculationDelegate = nullptr;

constexpr char ManagedPartDllName[] = "ManagedPart";
constexpr char SymbolReaderClassName[] = "NetCoreDbg.SymbolReader";
constexpr char EvaluationClassName[] = "NetCoreDbg.Evaluation";
constexpr char UtilsClassName[] = "NetCoreDbg.Utils";

// Pass to managed helper code to read in-memory PEs/PDBs
// Returns the number of bytes read.
int ReadMemoryForSymbols(uint64_t address, char *buffer, int cb)
{
    if (address == 0 || buffer == 0 || cb == 0)
        return 0;

    std::memcpy(buffer, (const void*) address, cb);
    return cb;
}

} // unnamed namespace

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

SequencePoint::~SequencePoint() noexcept
{
    Interop::SysFreeString(document);
}

void DisposeSymbols(PVOID pSymbolReaderHandle)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!disposeDelegate || !pSymbolReaderHandle)
        return;

    disposeDelegate(pSymbolReaderHandle);
}

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
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetHoistedLocalScopes", (void **)&getHoistedLocalScopesDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSequencePointByILOffset", (void **)&getSequencePointByILOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSequencePoints", (void **)&getSequencePointsDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetNextUserCodeILOffset", (void **)&getNextUserCodeILOffsetDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetModuleMethodsRanges", (void **)&getModuleMethodsRangesDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "ResolveBreakPoints", (void **)&resolveBreakPointsDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetAsyncMethodSteppingInfo", (void **)&getAsyncMethodSteppingInfoDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "GetSource", (void **)&getSourceDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, SymbolReaderClassName, "LoadDeltaPdb", (void **)&loadDeltaPdbDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "CalculationDelegate", (void **)&calculationDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "GenerateStackMachineProgram", (void **)&generateStackMachineProgramDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "ReleaseStackMachineProgram", (void **)&releaseStackMachineProgramDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, EvaluationClassName, "NextStackCommand", (void **)&nextStackCommandDelegate)) &&
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "StringToUpper", (void **)&stringToUpperDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "CoTaskMemAlloc", (void **)&coTaskMemAllocDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "CoTaskMemFree", (void **)&coTaskMemFreeDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "SysAllocStringLen", (void **)&sysAllocStringLenDelegate));
        SUCCEEDED(Status = createDelegate(hostHandle, domainId, ManagedPartDllName, UtilsClassName, "SysFreeString", (void **)&sysFreeStringDelegate));

    if (!allDelegatesCreated)
        throw std::runtime_error("createDelegate failed with status: " + std::to_string(Status));

    bool allDelegatesInited = loadSymbolsForModuleDelegate &&
                              disposeDelegate &&
                              getLocalVariableNameAndScopeDelegate &&
                              getHoistedLocalScopesDelegate &&
                              getSequencePointByILOffsetDelegate &&
                              getSequencePointsDelegate &&
                              getNextUserCodeILOffsetDelegate &&
                              getStepRangesFromIPDelegate &&
                              getModuleMethodsRangesDelegate &&
                              resolveBreakPointsDelegate &&
                              getAsyncMethodSteppingInfoDelegate &&
                              getSourceDelegate &&
                              loadDeltaPdbDelegate &&
                              generateStackMachineProgramDelegate &&
                              releaseStackMachineProgramDelegate &&
                              nextStackCommandDelegate &&
                              stringToUpperDelegate &&
                              coTaskMemAllocDelegate &&
                              coTaskMemFreeDelegate &&
                              sysAllocStringLenDelegate &&
                              sysFreeStringDelegate &&
                              calculationDelegate;

    if (!allDelegatesInited)
        throw std::runtime_error("Some delegates nulled");
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
    getHoistedLocalScopesDelegate = nullptr;
    getSequencePointByILOffsetDelegate = nullptr;
    getSequencePointsDelegate = nullptr;
    getNextUserCodeILOffsetDelegate = nullptr;
    getStepRangesFromIPDelegate = nullptr;
    getModuleMethodsRangesDelegate = nullptr;
    resolveBreakPointsDelegate = nullptr;
    getAsyncMethodSteppingInfoDelegate = nullptr;
    getSourceDelegate = nullptr;
    loadDeltaPdbDelegate = nullptr;
    stringToUpperDelegate = nullptr;
    coTaskMemAllocDelegate = nullptr;
    coTaskMemFreeDelegate = nullptr;
    sysAllocStringLenDelegate = nullptr;
    sysFreeStringDelegate = nullptr;
    calculationDelegate = nullptr;
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

HRESULT GetSequencePoints(PVOID pSymbolReaderHandle, mdMethodDef methodToken, SequencePoint **sequencePoints, int32_t &Count)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getSequencePointsDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    RetCode retCode = getSequencePointsDelegate(pSymbolReaderHandle, methodToken, (PVOID*)sequencePoints, &Count);

    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetNextUserCodeILOffset(PVOID pSymbolReaderHandle, mdMethodDef methodToken, ULONG32 ilOffset, ULONG32 &ilNextOffset, bool *noUserCodeFound)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getNextUserCodeILOffsetDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    int32_t NoUserCodeFound = 0;

    // Sequence points with startLine equal to 0xFEEFEE marker are filtered out on the managed side.
    RetCode retCode = getNextUserCodeILOffsetDelegate(pSymbolReaderHandle, methodToken, ilOffset, &ilNextOffset, &NoUserCodeFound);

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
                                      WCHAR *localName, ULONG localNameLen, ULONG32 *pIlStart, ULONG32 *pIlEnd)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getLocalVariableNameAndScopeDelegate || !pSymbolReaderHandle || !localName || !pIlStart || !pIlEnd)
        return E_FAIL;

    BSTR wszLocalName = Interop::SysAllocStringLen(mdNameLen);
    if (InteropPlatform::SysStringLen(wszLocalName) == 0)
        return E_OUTOFMEMORY;

    RetCode retCode = getLocalVariableNameAndScopeDelegate(pSymbolReaderHandle, methodToken, localIndex, &wszLocalName, pIlStart, pIlEnd);
    read_lock.unlock();

    if (retCode != RetCode::OK)
    {
        Interop::SysFreeString(wszLocalName);
        return E_FAIL;
    }

    wcscpy_s(localName, localNameLen, wszLocalName);
    Interop::SysFreeString(wszLocalName);

    return S_OK;
}

HRESULT GetHoistedLocalScopes(PVOID pSymbolReaderHandle, mdMethodDef methodToken, PVOID *data, int32_t &hoistedLocalScopesCount)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getHoistedLocalScopesDelegate || !pSymbolReaderHandle)
        return E_FAIL;

    RetCode retCode = getHoistedLocalScopesDelegate(pSymbolReaderHandle, methodToken, data, &hoistedLocalScopesCount);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT CalculationDelegate(PVOID firstOp, int32_t firstType, PVOID secondOp, int32_t secondType, int32_t operationType, int32_t &resultType, PVOID *data, std::string &errorText)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!calculationDelegate)
        return E_FAIL;

    BSTR werrorText;
    RetCode retCode = calculationDelegate(firstOp, firstType, secondOp, secondType, operationType, &resultType, data, &werrorText);
    read_lock.unlock();

    if (retCode != RetCode::OK)
    {
        errorText = to_utf8(werrorText);
        Interop::SysFreeString(werrorText);
        return E_FAIL;
    }

    return S_OK;
}

HRESULT GetModuleMethodsRanges(PVOID pSymbolReaderHandle, uint32_t constrTokensNum, PVOID constrTokens, uint32_t normalTokensNum, PVOID normalTokens, PVOID *data)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getModuleMethodsRangesDelegate || !pSymbolReaderHandle || (constrTokensNum && !constrTokens) || (normalTokensNum && !normalTokens) || !data)
        return E_FAIL;

    RetCode retCode = getModuleMethodsRangesDelegate(pSymbolReaderHandle, constrTokensNum, constrTokens, normalTokensNum, normalTokens, data);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT ResolveBreakPoints(PVOID pSymbolReaderHandles[], int32_t tokenNum, PVOID Tokens, int32_t sourceLine, int32_t nestedToken, int32_t &Count, const std::string &sourcePath, PVOID *data)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!resolveBreakPointsDelegate || !pSymbolReaderHandles || !Tokens || !data)
        return E_FAIL;

    RetCode retCode = resolveBreakPointsDelegate(pSymbolReaderHandles, tokenNum, Tokens, sourceLine, nestedToken, &Count, to_utf16(sourcePath).c_str(), data);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT GetAsyncMethodSteppingInfo(PVOID pSymbolReaderHandle, mdMethodDef methodToken, std::vector<AsyncAwaitInfoBlock> &AsyncAwaitInfo, ULONG32 *ilOffset)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getAsyncMethodSteppingInfoDelegate || !pSymbolReaderHandle || !ilOffset)
        return E_FAIL;

    AsyncAwaitInfoBlock *allocatedAsyncInfo = nullptr;
    int32_t asyncInfoCount = 0;

    RetCode retCode = getAsyncMethodSteppingInfoDelegate(pSymbolReaderHandle, methodToken, (PVOID*)&allocatedAsyncInfo, &asyncInfoCount, ilOffset);
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

HRESULT GenerateStackMachineProgram(const std::string &expr, PVOID *ppStackProgram, std::string &textOutput)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!generateStackMachineProgramDelegate || !ppStackProgram)
        return E_FAIL;

    textOutput = "";
    BSTR wTextOutput = nullptr;
    HRESULT Status = generateStackMachineProgramDelegate(to_utf16(expr).c_str(), ppStackProgram, &wTextOutput);
    read_lock.unlock();

    if (wTextOutput)
    {
        textOutput = to_utf8(wTextOutput);
        SysFreeString(wTextOutput);
    }

    return Status;
}

void ReleaseStackMachineProgram(PVOID pStackProgram)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!releaseStackMachineProgramDelegate || !pStackProgram)
        return;

    releaseStackMachineProgramDelegate(pStackProgram);
}

// Note, managed part will release Ptr unmanaged memory at object finalizer call after ReleaseStackMachineProgram() call.
// Native part must not release Ptr memory, allocated by managed part.
HRESULT NextStackCommand(PVOID pStackProgram, int32_t &Command, PVOID &Ptr, std::string &textOutput)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!nextStackCommandDelegate || !pStackProgram)
        return E_FAIL;

    textOutput = "";
    BSTR wTextOutput = nullptr;
    HRESULT Status = nextStackCommandDelegate(pStackProgram, &Command, &Ptr, &wTextOutput);
    read_lock.unlock();

    if (wTextOutput)
    {
        textOutput = to_utf8(wTextOutput);
        SysFreeString(wTextOutput);
    }

    return Status;
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

HRESULT GetSource(PVOID symbolReaderHandle, std::string fileName, PVOID *data, int32_t *length)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!getSourceDelegate || !symbolReaderHandle)
        return E_FAIL;

    RetCode retCode = getSourceDelegate(symbolReaderHandle, to_utf16(fileName).c_str(), length, data);
    return retCode == RetCode::OK ? S_OK : E_FAIL;
}

HRESULT LoadDeltaPdb(const std::string &pdbPath, VOID **ppSymbolReaderHandle, std::unordered_set<mdMethodDef> &methodTokens)
{
    std::unique_lock<Utility::RWLock::Reader> read_lock(CLRrwlock.reader);
    if (!loadDeltaPdbDelegate|| !ppSymbolReaderHandle || pdbPath.empty())
        return E_FAIL;

    PVOID pMethodTokens = nullptr;
    int32_t tokensCount = 0;

    *ppSymbolReaderHandle = loadDeltaPdbDelegate(to_utf16(pdbPath).c_str(), &pMethodTokens, &tokensCount);

    if (tokensCount > 0 && pMethodTokens)
    {
        for(int i = 0; i < tokensCount; i++)
        {
            methodTokens.insert(((mdMethodDef*)pMethodTokens)[i]);
        }
    }

    if (pMethodTokens)
        CoTaskMemFree(pMethodTokens);

    if (*ppSymbolReaderHandle == 0)
        return E_FAIL;

    return S_OK;
}

} // namespace Interop

} // namespace netcoredbg
