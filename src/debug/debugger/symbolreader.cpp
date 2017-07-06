#include <sys/stat.h>
#include <linux/limits.h>
#include <dlfcn.h>

#include <unistd.h>

#include <windows.h>
#include <coreclrhost.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"
#include "clrinternal.h"

#include "arrayholder.h"

#include <string>

#include "torelease.h"
#include "symbolreader.h"

std::string SymbolReader::coreClrPath;
LoadSymbolsForModuleDelegate SymbolReader::loadSymbolsForModuleDelegate;
DisposeDelegate SymbolReader::disposeDelegate;
ResolveSequencePointDelegate SymbolReader::resolveSequencePointDelegate;
GetLocalVariableName SymbolReader::getLocalVariableNameDelegate;
GetLineByILOffsetDelegate SymbolReader::getLineByILOffsetDelegate;
GetStepRangesFromIPDelegate SymbolReader::getStepRangesFromIPDelegate;

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

    ULONG32 len = 0;
    WCHAR moduleName[MAX_LONGPATH];
    IfFailRet(pModule->GetName(_countof(moduleName), &len, moduleName));

    return LoadSymbolsForPortablePDB(moduleName, isInMemory, isInMemory, peAddress, peSize, 0, 0);
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
    WCHAR* pModuleName,
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
    ArrayHolder<char> szModuleName = nullptr;
    if (!isInMemory && pModuleName != nullptr)
    {
        szModuleName = new char[MAX_LONGPATH];
        if (WideCharToMultiByte(CP_ACP, 0, pModuleName, (int)(_wcslen(pModuleName) + 1), szModuleName, MAX_LONGPATH, NULL, NULL) == 0)
        {
            return E_FAIL;
        }
    }

    m_symbolReaderHandle = loadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress,
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (m_symbolReaderHandle == 0)
    {
        return E_FAIL;
    }

    return Status;
}

void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList);

static std::string GetExeAbsPath()
{
    static const char* self_link = "/proc/self/exe";

    char exe[PATH_MAX];

    ssize_t r = readlink(self_link, exe, PATH_MAX - 1);

    if (r < 0)
    {
        return std::string();
    }

    exe[r] = '\0';

    return exe;
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

    WCHAR wszCoreClrPath[MAX_LONGPATH];
    MultiByteToWideChar(CP_UTF8, 0, coreClrPath.c_str(), coreClrPath.size() + 1, wszCoreClrPath, MAX_LONGPATH);

    std::string clrDir = coreClrPath.substr(0, coreClrPath.rfind('/'));

    HRESULT Status;

    //coreClrPath = g_ExtServices->GetCoreClrDirectory();
    // if (!GetAbsolutePath(coreClrPath, absolutePath))
    // {
    //     //ExtErr("Error: Failed to get coreclr absolute path\n");
    //     return E_FAIL;
    // }
    // coreClrPath.append(DIRECTORY_SEPARATOR_STR_A);
    // coreClrPath.append(MAIN_CLR_DLL_NAME_A);

    HMODULE coreclrLib = LoadLibraryW(wszCoreClrPath);
    if (coreclrLib == nullptr)
    {
        printf("Error: Failed to load coreclr\n");
        return E_FAIL;
    }

    void *hostHandle;
    unsigned int domainId;
    coreclr_initialize_ptr initializeCoreCLR = (coreclr_initialize_ptr)GetProcAddress(coreclrLib, "coreclr_initialize");
    if (initializeCoreCLR == nullptr)
    {
        printf("Error: coreclr_initialize not found\n");
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
        printf("GetExeAbsPath is empty\n");
        return E_FAIL;
    }

    std::size_t dirSepIndex = exe.rfind('/');
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

    // std::string entryPointExecutablePath;
    // if (!GetEntrypointExecutableAbsolutePath(entryPointExecutablePath))
    // {
    //     //ExtErr("Could not get full path to current executable");
    //     return E_FAIL;
    // }

    Status = initializeCoreCLR(exe.c_str(), "debugger",
        sizeof(propertyKeys) / sizeof(propertyKeys[0]), propertyKeys, propertyValues, &hostHandle, &domainId);

    if (FAILED(Status))
    {
        printf("Error: Fail to initialize CoreCLR %08x\n", Status);
        return Status;
    }

    coreclr_create_delegate_ptr createDelegate = (coreclr_create_delegate_ptr)GetProcAddress(coreclrLib, "coreclr_create_delegate");
    if (createDelegate == nullptr)
    {
        printf("Error: coreclr_create_delegate not found\n");
        return E_FAIL;
    }

    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "LoadSymbolsForModule", (void **)&loadSymbolsForModuleDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "Dispose", (void **)&disposeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "ResolveSequencePoint", (void **)&resolveSequencePointDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLocalVariableName", (void **)&getLocalVariableNameDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLineByILOffset", (void **)&getLineByILOffsetDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate));

    return Status;
}

HRESULT SymbolReader::ResolveSequencePoint(WCHAR* pFilename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(resolveSequencePointDelegate != nullptr);

        char szName[mdNameLen];
        if (WideCharToMultiByte(CP_ACP, 0, pFilename, (int)(_wcslen(pFilename) + 1), szName, mdNameLen, NULL, NULL) == 0)
        {
            return E_FAIL;
        }
        if (resolveSequencePointDelegate(m_symbolReaderHandle, szName, lineNumber, pToken, pIlOffset) == FALSE)
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

        BSTR bstrFileName = SysAllocStringLen(0, MAX_LONGPATH);
        if (bstrFileName == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        // Source lines with 0xFEEFEE markers are filtered out on the managed side.
        if ((getLineByILOffsetDelegate(m_symbolReaderHandle, methodToken, ilOffset, pLinenum, &bstrFileName) == FALSE) || (*pLinenum == 0))
        {
            SysFreeString(bstrFileName);
            return E_FAIL;
        }
        wcscpy_s(pwszFileName, cchFileName, bstrFileName);
        SysFreeString(bstrFileName);
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
