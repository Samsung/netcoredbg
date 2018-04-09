// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "symbolreader.h"

#include <coreclrhost.h>

#include "modules.h"
#include "platform.h"
#include "torelease.h"
#include "cputil.h"


static const char *SymbolReaderDllName = "SymbolReader";
static const char *SymbolReaderClassName = "SOS.SymbolReader";

// Suppress undefined reference
// `_invalid_parameter(char16_t const*, char16_t const*, char16_t const*, unsigned int, unsigned long)':
//      /coreclr/src/pal/inc/rt/safecrt.h:386: undefined reference to `RaiseException'
static void RaiseException(DWORD dwExceptionCode,
               DWORD dwExceptionFlags,
               DWORD nNumberOfArguments,
               CONST ULONG_PTR *lpArguments)
{
}

std::string SymbolReader::coreClrPath;
LoadSymbolsForModuleDelegate SymbolReader::loadSymbolsForModuleDelegate;
DisposeDelegate SymbolReader::disposeDelegate;
ResolveSequencePointDelegate SymbolReader::resolveSequencePointDelegate;
GetLocalVariableNameAndScope SymbolReader::getLocalVariableNameAndScopeDelegate;
GetLineByILOffsetDelegate SymbolReader::getLineByILOffsetDelegate;
GetStepRangesFromIPDelegate SymbolReader::getStepRangesFromIPDelegate;
GetSequencePointsDelegate SymbolReader::getSequencePointsDelegate;

SysAllocStringLen_t SymbolReader::sysAllocStringLen;
SysFreeString_t SymbolReader::sysFreeString;
SysStringLen_t SymbolReader::sysStringLen;
CoTaskMemFree_t SymbolReader::coTaskMemFree;

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
    const char *szModuleName = nullptr;
    if (!isInMemory && !modulePath.empty())
    {
        szModuleName = modulePath.c_str();
    }

    m_symbolReaderHandle = loadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress,
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (m_symbolReaderHandle == 0)
    {
        return E_FAIL;
    }

    return Status;
}

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

    sysAllocStringLen = (SysAllocStringLen_t)DLSym(coreclrLib, "SysAllocStringLen");
    sysFreeString = (SysFreeString_t)DLSym(coreclrLib, "SysFreeString");
    sysStringLen = (SysStringLen_t)DLSym(coreclrLib, "SysStringLen");
    coTaskMemFree = (CoTaskMemFree_t)DLSym(coreclrLib, "CoTaskMemFree");

    if (sysAllocStringLen == nullptr)
    {
        fprintf(stderr, "Error: SysAllocStringLen not found\n");
        return E_FAIL;
    }

    if (sysFreeString == nullptr)
    {
        fprintf(stderr, "Error: SysFreeString not found\n");
        return E_FAIL;
    }

    if (sysStringLen == nullptr)
    {
        fprintf(stderr, "Error: SysStringLen not found\n");
        return E_FAIL;
    }

    if (coTaskMemFree == nullptr)
    {
        fprintf(stderr, "Error: CoTaskMemFree not found\n");
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
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLineByILOffset", (void **)&getLineByILOffsetDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetSequencePoints", (void **)&getSequencePointsDelegate));

    return Status;
}

HRESULT SymbolReader::ResolveSequencePoint(const char *filename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(resolveSequencePointDelegate != nullptr);

        if (resolveSequencePointDelegate(m_symbolReaderHandle, filename, lineNumber, pToken, pIlOffset) == FALSE)
        {
            return E_FAIL;
        }
        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetLineByILOffset(
    mdMethodDef methodToken,
    ULONG64 ilOffset,
    ULONG *pLinenum,
    WCHAR* pwszFileName,
    ULONG cchFileName)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getLineByILOffsetDelegate != nullptr);

        BSTR bstrFileName = sysAllocStringLen(0, MAX_LONGPATH);
        if (sysStringLen(bstrFileName) == 0)
        {
            return E_OUTOFMEMORY;
        }
        // Source lines with 0xFEEFEE markers are filtered out on the managed side.
        if ((getLineByILOffsetDelegate(m_symbolReaderHandle, methodToken, ilOffset, pLinenum, &bstrFileName) == FALSE) || (*pLinenum == 0))
        {
            sysFreeString(bstrFileName);
            return E_FAIL;
        }
        wcscpy_s(pwszFileName, cchFileName, bstrFileName);
        sysFreeString(bstrFileName);
        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetStepRangesFromIP(ULONG64 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset)
{
    HRESULT Status = S_OK;

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
    HRESULT Status = S_OK;

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
    HRESULT Status = S_OK;

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
