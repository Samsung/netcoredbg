// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules.h"

#include <sstream>
#include <vector>
#include <iomanip>

#include "managed/interop.h"
#include "utils/platform.h"
#include "metadata/typeprinter.h"
#include "metadata/jmc.h"
#include "utils/filesystem.h"

namespace netcoredbg
{

Modules::ModuleInfo::~ModuleInfo() noexcept
{
    for (auto symbolReaderHandle : m_symbolReaderHandles)
    {
        if (symbolReaderHandle != nullptr)
            Interop::DisposeSymbols(symbolReaderHandle);
    }
}

static bool IsTargetFunction(const std::vector<std::string> &fullName, const std::vector<std::string> &targetName)
{
    // Function should be matched by substring, i.e. received target function name should fully or partly equal with the
    // real function name. For example:
    //
    // "MethodA" matches
    // Program.ClassA.MethodA
    // Program.ClassB.MethodA
    // Program.ClassA.InnerClass.MethodA
    //
    // "ClassA.MethodB" matches
    // Program.ClassA.MethodB
    // Program.ClassB.ClassA.MethodB

    auto fullIt = fullName.rbegin();
    for (auto it = targetName.rbegin(); it != targetName.rend(); it++)
    {
        if (fullIt == fullName.rend() || *it != *fullIt)
            return false;

        fullIt++;
    }

    return true;
}

static HRESULT ForEachMethod(ICorDebugModule *pModule, std::function<bool(const std::string&, mdMethodDef&)> functor)
{
    HRESULT Status;
    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;

    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID *)&pMDImport));

    ULONG typesCnt = 0;
    HCORENUM fTypeEnum = NULL;
    mdTypeDef mdType = mdTypeDefNil;

    while (SUCCEEDED(pMDImport->EnumTypeDefs(&fTypeEnum, &mdType, 1, &typesCnt)) && typesCnt != 0)
    {
        std::string typeName;
        IfFailRet(TypePrinter::NameForToken(mdType, pMDImport, typeName, false, nullptr));

        HCORENUM fFuncEnum = NULL;
        mdMethodDef mdMethod = mdMethodDefNil;
        ULONG methodsCnt = 0;

        while (SUCCEEDED(pMDImport->EnumMethods(&fFuncEnum, mdType, &mdMethod, 1, &methodsCnt)) && methodsCnt != 0)
        {
            mdTypeDef memTypeDef;
            ULONG nameLen;
            WCHAR szFuncName[mdNameLen] = {0};

            Status = pMDImport->GetMethodProps(mdMethod, &memTypeDef, szFuncName, _countof(szFuncName), &nameLen,
                                               nullptr, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(Status))
                continue;

            // Get generic types
            ToRelease<IMetaDataImport2> pMDImport2;

            IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport2, (LPVOID *)&pMDImport2));

            HCORENUM fGenEnum = NULL;
            mdGenericParam gp;
            ULONG fetched;
            std::string genParams("");

            while (SUCCEEDED(pMDImport2->EnumGenericParams(&fGenEnum, mdMethod, &gp, 1, &fetched)) && fetched == 1)
            {
                mdMethodDef memMethodDef;
                WCHAR szGenName[mdNameLen] = {0};
                ULONG genNameLen;

                Status = pMDImport2->GetGenericParamProps(gp, nullptr, nullptr, &memMethodDef, nullptr, szGenName, _countof(szGenName), &genNameLen);
                if (FAILED(Status))
                    continue;

                // Add comma for each element. The last one will be stripped later.
                genParams += to_utf8(szGenName) + ",";
            }

            pMDImport2->CloseEnum(fGenEnum);

            std::string fullName = to_utf8(szFuncName);
            if (genParams != "")
            {
                // Last symbol is comma and it is useless, so remove
                genParams.pop_back();
                fullName += "<" + genParams + ">";
            }

            if (!functor(typeName + "." + fullName, mdMethod))
            {
                pMDImport->CloseEnum(fFuncEnum);
                pMDImport->CloseEnum(fTypeEnum);
                return E_FAIL;
            }
        }

        pMDImport->CloseEnum(fFuncEnum);
    }
    pMDImport->CloseEnum(fTypeEnum);

    return S_OK;
}

static std::vector<std::string> split_on_tokens(const std::string &str, const char delim)
{
    std::vector<std::string> res;
    size_t pos = 0, prev = 0;

    while (true)
    {
        pos = str.find(delim, prev);
        if (pos == std::string::npos)
        {
            res.push_back(std::string(str, prev));
            break;
        }

        res.push_back(std::string(str, prev, pos - prev));
        prev = pos + 1;
    }

    return res;
}

static HRESULT ResolveMethodInModule(ICorDebugModule *pModule, const std::string &funcName, ResolveFuncBreakpointCallback cb)
{
    std::vector<std::string> splittedName = split_on_tokens(funcName, '.');

    auto functor = [&](const std::string& fullName, mdMethodDef& mdMethod) -> bool
    {
        std::vector<std::string> splittedFullName = split_on_tokens(fullName, '.');

        // If we've found the target function
        if (IsTargetFunction(splittedFullName, splittedName))
        {
            if (FAILED(cb(pModule, mdMethod)))
                return false; // abort operation
        }

        return true;  // continue for other functions with matching name
    };

    return ForEachMethod(pModule, functor);
}

void Modules::CleanupAllModules()
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesInfo.clear();
    m_modulesAppUpdate.Clear();
}

std::string GetModuleFileName(ICorDebugModule *pModule)
{
    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;

    if (FAILED(pModule->GetName(_countof(name), &name_len, name)))
        return std::string();

    std::string moduleName = to_utf8(name/*, name_len*/);

    // On Tizen platform module path may look like /proc/self/fd/8/bin/Xamarin.Forms.Platform.dll
    // This path is invalid in debugger process, we shoud change `self` to `<debugee process id>`
    static const std::string selfPrefix("/proc/self/");

    if (moduleName.compare(0, selfPrefix.size(), selfPrefix) != 0)
        return moduleName;

    ToRelease<ICorDebugProcess> pProcess;
    if (FAILED(pModule->GetProcess(&pProcess)))
        return std::string();

    DWORD pid = 0;

    if (FAILED(pProcess->GetID(&pid)))
        return std::string();

    std::ostringstream ss;
    ss << "/proc/" << pid << "/" << moduleName.substr(selfPrefix.size());
    return ss.str();
}

static std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

HRESULT IsModuleHaveSameName(ICorDebugModule *pModule, const std::string &Name, bool isFullPath)
{
    HRESULT Status;
    ULONG32 len;
    WCHAR szModuleName[mdNameLen] = {0};
    std::string modName;

    IfFailRet(pModule->GetName(_countof(szModuleName), &len, szModuleName));

    if (isFullPath)
        modName = to_utf8(szModuleName);
    else
        modName = GetBasename(to_utf8(szModuleName));

    return modName == Name ? S_OK : S_FALSE;
}

HRESULT Modules::GetModuleInfo(CORDB_ADDRESS modAddress, ModuleInfoCallback cb)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    return (info_pair == m_modulesInfo.end()) ? E_FAIL : cb(info_pair->second);
}

// Caller must care about m_modulesInfoMutex.
HRESULT Modules::GetModuleInfo(CORDB_ADDRESS modAddress, ModuleInfo **ppmdInfo)
{
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
        return E_FAIL;

    *ppmdInfo = &info_pair->second;
    return S_OK;
}

HRESULT Modules::ResolveFuncBreakpointInAny(const std::string &module,
                                            bool &module_checked,
                                            const std::string &funcname,
                                            ResolveFuncBreakpointCallback cb)
{
    bool isFullPath = IsFullPath(module);
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        ICorDebugModule *pModule = mdInfo.m_iCorModule.GetPtr();

        if (!module.empty())
        {
            IfFailRet(IsModuleHaveSameName(pModule, module, isFullPath));
            if (Status == S_FALSE)
                continue;

            module_checked = true;
        }

        ResolveMethodInModule(mdInfo.m_iCorModule, funcname, cb);

        if (module_checked)
            break;
    }

    return S_OK;
}


HRESULT Modules::ResolveFuncBreakpointInModule(ICorDebugModule *pModule, const std::string &module, bool &module_checked,
                                               std::string &funcname, ResolveFuncBreakpointCallback cb)
{
    HRESULT Status;

    if (!module.empty())
    {
        IfFailRet(IsModuleHaveSameName(pModule, module, IsFullPath(module)));
        if (Status == S_FALSE)
            return E_FAIL;

        module_checked = true;
    }

    return ResolveMethodInModule(pModule, funcname, cb);
}

HRESULT Modules::GetFrameILAndSequencePoint(
    ICorDebugFrame *pFrame,
    ULONG32 &ilOffset,
    Modules::SequencePoint &sequencePoint)
{
    HRESULT Status;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));
    ULONG32 methodVersion;
    IfFailRet(pCode->GetVersionNumber(&methodVersion));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    return GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        IfFailRet(GetSequencePointByILOffset(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, ilOffset, &sequencePoint));

        // In case Hot Reload we may have line updates that we must take into account.
        unsigned fullPathIndex;
        IfFailRet(GetIndexBySourceFullPath(sequencePoint.document, fullPathIndex));
        LineUpdatesForwardCorrection(fullPathIndex, methodToken, mdInfo.m_methodBlockUpdates, sequencePoint);

        return S_OK;
    });
}

HRESULT Modules::GetFrameILAndNextUserCodeILOffset(
    ICorDebugFrame *pFrame,
    ULONG32 &ilOffset,
    ULONG32 &ilNextOffset,
    bool *noUserCodeFound)
{
    HRESULT Status;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));
    ULONG32 methodVersion;
    IfFailRet(pCode->GetVersionNumber(&methodVersion));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    return GetNextUserCodeILOffsetInMethod(pModule, methodToken, methodVersion, ilOffset, ilNextOffset, noUserCodeFound);
}

HRESULT Modules::GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));
    ULONG32 methodVersion;
    IfFailRet(pCode->GetVersionNumber(&methodVersion));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 nOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&nOffset, &mappingResult));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    ULONG32 ilStartOffset;
    ULONG32 ilEndOffset;

    IfFailRet(GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        return Interop::GetStepRangesFromIP(mdInfo.m_symbolReaderHandles[methodVersion - 1], nOffset, methodToken, &ilStartOffset, &ilEndOffset);
    }));

    if (ilStartOffset == ilEndOffset)
    {
        ToRelease<ICorDebugCode> pCode;
        IfFailRet(pFunc->GetILCode(&pCode));
        IfFailRet(pCode->GetSize(&ilEndOffset));
    }

    range->startOffset = ilStartOffset;
    range->endOffset = ilEndOffset;

    return S_OK;
}

HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    GUID mvid;

    IfFailRet(pMDImport->GetScopeProps(nullptr, 0, nullptr, &mvid));

    std::ostringstream ss;
    ss << std::hex
    << std::setfill('0') << std::setw(8) << mvid.Data1 << "-"
    << std::setfill('0') << std::setw(4) << mvid.Data2 << "-"
    << std::setfill('0') << std::setw(4) << mvid.Data3 << "-"
    << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[0]) & 0xFF)
    << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[1]) & 0xFF)
    << "-";
    for (int i = 2; i < 8; i++)
        ss << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[i]) & 0xFF);

    id = ss.str();

    return S_OK;
}

static HRESULT LoadSymbols(IMetaDataImport *pMD, ICorDebugModule *pModule, ULONG64 inMemoryPdbAddress, ULONG64 inMemoryPdbSize, VOID **ppSymbolReaderHandle)
{
    HRESULT Status = S_OK;
    BOOL isDynamic = FALSE;
    BOOL isInMemory = FALSE;
    IfFailRet(pModule->IsDynamic(&isDynamic));
    IfFailRet(pModule->IsInMemory(&isInMemory));

    if (isDynamic)
        return E_FAIL; // Dynamic assemblies are a special case which we will ignore for now

    std::vector<unsigned char> peBuf;
    ULONG64 peBufAddress = 0;
    ULONG32 peBufSize = 0;
    if (isInMemory)
    {
      ICorDebugProcess* process = 0;
      ULONG64 peAddress = 0;
      ULONG32 peSize = 0;
      IfFailRet(pModule->GetProcess(&process));
      IfFailRet(pModule->GetBaseAddress(&peAddress));
      IfFailRet(pModule->GetSize(&peSize));

      if (peAddress != 0 && peSize != 0)
      {
        peBufSize = peSize;
        peBuf.resize(peBufSize);
        peBufAddress = (ULONG64)&peBuf[0];
        SIZE_T read = 0;
        IfFailRet(process->ReadMemory(peAddress, peSize, &peBuf[0], &read));
        if (read != peSize)
          return E_FAIL;
      }
    }

    return Interop::LoadSymbolsForPortablePDB(
        GetModuleFileName(pModule),
        isInMemory,
        isInMemory, // isFileLayout
        peBufAddress,
        peBufSize,
        inMemoryPdbAddress,
        inMemoryPdbSize,
        ppSymbolReaderHandle
    );
}

HRESULT Modules::TryLoadModuleSymbols(ICorDebugModule *pModule, Module &module, bool needJMC, bool needHotReload, ULONG64 inMemoryPdbAddress, ULONG64 inMemoryPdbSize, std::string &outputText)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    module.path = GetModuleFileName(pModule);
    module.name = GetFileName(module.path);

    PVOID pSymbolReaderHandle = nullptr;
    LoadSymbols(pMDImport, pModule, inMemoryPdbAddress, inMemoryPdbSize, &pSymbolReaderHandle);
    module.symbolStatus = pSymbolReaderHandle != nullptr ? SymbolsLoaded : SymbolsNotFound;

    if (module.symbolStatus == SymbolsLoaded)
    {
        ToRelease<ICorDebugModule2> pModule2;
        if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
        {
            if (needHotReload)
                pModule2->SetJITCompilerFlags(CORDEBUG_JIT_ENABLE_ENC);
            else if (!needJMC) // Note, CORDEBUG_JIT_DISABLE_OPTIMIZATION is part of CORDEBUG_JIT_ENABLE_ENC.
                pModule2->SetJITCompilerFlags(CORDEBUG_JIT_DISABLE_OPTIMIZATION);

            if (SUCCEEDED(Status = pModule2->SetJMCStatus(TRUE, 0, nullptr))) // If we can't enable JMC for module, no reason disable JMC on module's types/methods.
            {
                // Note, we use JMC in runtime all the time (same behaviour as MS vsdbg and MSVS debugger have),
                // since this is the only way provide good speed for stepping in case "JMC disabled".
                // But in case "JMC disabled", debugger must care about different logic for exceptions/stepping/breakpoints.

                // https://docs.microsoft.com/en-us/visualstudio/debugger/just-my-code
                // The .NET debugger considers optimized binaries and non-loaded .pdb files to be non-user code.
                // Three compiler attributes also affect what the .NET debugger considers to be user code:
                // * DebuggerNonUserCodeAttribute tells the debugger that the code it's applied to isn't user code.
                // * DebuggerHiddenAttribute hides the code from the debugger, even if Just My Code is turned off.
                // * DebuggerStepThroughAttribute tells the debugger to step through the code it's applied to, rather than step into the code.
                // The .NET debugger considers all other code to be user code.
                if (needJMC)
                    DisableJMCByAttributes(pModule);
            }
            else if (Status == CORDBG_E_CANT_SET_TO_JMC)
            {
                if (needJMC)
                    outputText = "You are debugging a Release build of " + module.name + ". Using Just My Code with Release builds using compiler optimizations results in a degraded debugging experience (e.g. breakpoints will not be hit).";
                else
                    outputText = "You are debugging a Release build of " + module.name + ". Without Just My Code Release builds try not to use compiler optimizations, but in some cases (e.g. attach) this still results in a degraded debugging experience (e.g. breakpoints will not be hit).";
            }
        }

        if (FAILED(m_modulesSources.FillSourcesCodeLinesForModule(pModule, pMDImport, pSymbolReaderHandle)))
            LOGE("Could not load source lines related info from PDB file. Could produce failures during breakpoint's source path resolve in future.");
    }

    IfFailRet(GetModuleId(pModule, module.id));

    CORDB_ADDRESS baseAddress;
    ULONG32 size;
    IfFailRet(pModule->GetBaseAddress(&baseAddress));
    IfFailRet(pModule->GetSize(&size));
    module.baseAddress = baseAddress;
    module.size = size;

    pModule->AddRef();
    ModuleInfo mdInfo { pSymbolReaderHandle, pModule };
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesInfo.erase(baseAddress);
    m_modulesInfo.insert(std::make_pair(baseAddress, std::move(mdInfo)));

    if (needHotReload)
        IfFailRet(m_modulesAppUpdate.AddUpdateHandlerTypesForModule(pModule, pMDImport));

    return S_OK;
}

HRESULT Modules::GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG32 methodVersion,
    ULONG localIndex,
    WSTRING &localName,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    WCHAR wLocalName[mdNameLen] = W("\0");

    IfFailRet(GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        return Interop::GetNamedLocalVariableAndScope(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, localIndex, wLocalName, _countof(wLocalName), pIlStart, pIlEnd);
    }));

    localName = wLocalName;

    return S_OK;
}

HRESULT Modules::GetHoistedLocalScopes(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG32 methodVersion,
    PVOID *data,
    int32_t &hoistedLocalScopesCount)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    return GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        return Interop::GetHoistedLocalScopes(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, data, hoistedLocalScopesCount);
    });
}

HRESULT Modules::GetModuleWithName(const std::string &name, ICorDebugModule **ppModule, bool onlyWithPDB)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        std::string path = GetModuleFileName(mdInfo.m_iCorModule);

        if (onlyWithPDB && mdInfo.m_symbolReaderHandles.empty())
            continue;

        if (GetFileName(path) == name)
        {
            mdInfo.m_iCorModule->AddRef();
            *ppModule = mdInfo.m_iCorModule;
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT Modules::GetNextUserCodeILOffsetInMethod(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG32 methodVersion,
    ULONG32 ilOffset,
    ULONG32 &ilNextOffset,
    bool *noUserCodeFound)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    return GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        return Interop::GetNextUserCodeILOffset(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, ilOffset, ilNextOffset, noUserCodeFound);
    });
}

HRESULT Modules::GetSequencePointByILOffset(
    PVOID pSymbolReaderHandle,
    mdMethodDef methodToken,
    ULONG32 ilOffset,
    SequencePoint *sequencePoint)
{
    Interop::SequencePoint symSequencePoint;

    if (FAILED(Interop::GetSequencePointByILOffset(pSymbolReaderHandle, methodToken, ilOffset, &symSequencePoint))) {
        return E_FAIL;
    }

    sequencePoint->document = to_utf8(symSequencePoint.document);
    sequencePoint->startLine = symSequencePoint.startLine;
    sequencePoint->startColumn = symSequencePoint.startColumn;
    sequencePoint->endLine = symSequencePoint.endLine;
    sequencePoint->endColumn = symSequencePoint.endColumn;
    sequencePoint->offset = symSequencePoint.offset;

    return S_OK;
}

HRESULT Modules::GetSequencePointByILOffset(
    CORDB_ADDRESS modAddress,
    mdMethodDef methodToken,
    ULONG32 methodVersion,
    ULONG32 ilOffset,
    Modules::SequencePoint &sequencePoint)
{
    return GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
            return E_FAIL;

        return GetSequencePointByILOffset(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodToken, ilOffset, &sequencePoint);
    });
}

HRESULT Modules::ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb)
{
    HRESULT Status;
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        IfFailRet(cb(mdInfo.m_iCorModule));
    }
    return S_OK;
}

HRESULT Modules::ResolveBreakpoint(/*in*/ CORDB_ADDRESS modAddress, /*in*/ std::string filename, /*out*/ unsigned &fullname_index,
                                   /*in*/ int sourceLine, /*out*/ std::vector<ModulesSources::resolved_bp_t> &resolvedPoints)
{
#ifdef WIN32
    HRESULT Status;
    IfFailRet(Interop::StringToUpper(filename));
#endif

    // Note, in all code we use m_modulesInfoMutex > m_sourcesInfoMutex lock sequence.
    std::lock_guard<std::mutex> lockModulesInfo(m_modulesInfoMutex);
    return m_modulesSources.ResolveBreakpoint(this, modAddress, filename, fullname_index, sourceLine, resolvedPoints);
}

HRESULT Modules::ApplyPdbDeltaAndLineUpdates(ICorDebugModule *pModule, bool needJMC, const std::string &deltaPDB,
                                             const std::string &lineUpdates, std::unordered_set<mdMethodDef> &methodTokens)
{
    return m_modulesSources.ApplyPdbDeltaAndLineUpdates(this, pModule, needJMC, deltaPDB, lineUpdates, methodTokens);
}

HRESULT Modules::GetSourceFullPathByIndex(unsigned index, std::string &fullPath)
{
    return m_modulesSources.GetSourceFullPathByIndex(index, fullPath);
}

HRESULT Modules::GetIndexBySourceFullPath(std::string fullPath, unsigned &index)
{
    return m_modulesSources.GetIndexBySourceFullPath(fullPath, index);
}

void Modules::FindFileNames(string_view pattern, unsigned limit, std::function<void(const char *)> cb)
{
    m_modulesSources.FindFileNames(pattern, limit, cb);
}

void Modules::FindFunctions(Utility::string_view pattern, unsigned limit, std::function<void(const char *)> cb)
{
    auto functor = [&](const std::string& fullName, mdMethodDef &)
    {
        if (limit == 0)
            return false; // limit exceeded

        auto pos = fullName.find(pattern);
        if (pos != std::string::npos && (pos == 0 || fullName[pos-1] == '.'))
        {
            limit--;
            cb(fullName.c_str());
        }

        return true;  // continue for next functions
    }; 

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    for (const auto& modpair : m_modulesInfo)
    {
        HRESULT Status = ForEachMethod(modpair.second.m_iCorModule, functor);
        if (FAILED(Status))
            break;
    }
}

HRESULT Modules::GetSource(ICorDebugModule *pModule, const std::string &sourcePath, char** fileBuf, int* fileLen)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    return GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.size() > 1)
        {
            LOGE("This feature not support simultaneous work with Hot Reload.");
            return E_FAIL;
        }

        return mdInfo.m_symbolReaderHandles.empty() ?
            E_FAIL :
            Interop::GetSource(mdInfo.m_symbolReaderHandles[0], sourcePath, (PVOID*)fileBuf, fileLen);
    });
}

void Modules::CopyModulesUpdateHandlerTypes(std::vector<ToRelease<ICorDebugType>> &modulesUpdateHandlerTypes)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesAppUpdate.CopyModulesUpdateHandlerTypes(modulesUpdateHandlerTypes);
}

} // namespace netcoredbg
