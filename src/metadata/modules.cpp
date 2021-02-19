// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules.h"

#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>

#include "managed/interop.h"
#include "platform.h"
#include "utils/utf.h"
#include "metadata/typeprinter.h"

namespace netcoredbg
{

namespace
{
    std::vector<std::string> split_on_tokens(const std::string &str, const char delim)
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
}


bool Modules::IsTargetFunction(const std::vector<std::string> &fullName, const std::vector<std::string> &targetName)
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


HRESULT Modules::ResolveMethodInModule(ICorDebugModule *pModule, const std::string &funcName,
                                       ResolveFunctionBreakpointCallback cb)
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


HRESULT Modules::ForEachMethod(ICorDebugModule *pModule, std::function<bool(const std::string&, mdMethodDef&)> functor)
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
        std::list<std::string> args;

        IfFailRet(TypePrinter::NameForToken(mdType, pMDImport, typeName, false, args));

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

void Modules::CleanupAllModules()
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesInfo.clear();
}

std::string Modules::GetModuleFileName(ICorDebugModule *pModule)
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

string_view Modules::GetFileName(string_view path)
{
    size_t i = path.find_last_of("/\\");
    return i == string_view::npos ? path : path.substr(i + 1);
}

HRESULT Modules::GetLocationInAny(
    std::string filename,
    ULONG linenum,
    ULONG32 &ilOffset,
    mdMethodDef &methodToken,
    ICorDebugModule **ppModule)
{
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        CORDB_ADDRESS modAddress;
        IfFailRet(mdInfo.module->GetBaseAddress(&modAddress));
        if (FAILED(mdInfo.managedPart->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset)))
            continue;

        SequencePoint resolvedSequencePoint;
        if (FAILED(GetSequencePointByILOffset(mdInfo.managedPart.get(), methodToken, ilOffset, &resolvedSequencePoint)))
            continue;

        mdInfo.module->AddRef();
        *ppModule = mdInfo.module.GetPtr();
        return S_OK;
    }
    return E_FAIL;
}

HRESULT Modules::GetLocationInModule(
    ICorDebugModule *pModule,
    std::string filename,
    ULONG linenum,
    ULONG32 &ilOffset,
    mdMethodDef &methodToken)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(info_pair->second.managedPart->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset));

    SequencePoint resolvedSequencePoint;
    IfFailRet(GetSequencePointByILOffset(info_pair->second.managedPart.get(), methodToken, ilOffset, &resolvedSequencePoint));

    return S_OK;
}

HRESULT Modules::ResolveFunctionInAny(const std::string &module,
                                      const std::string &funcname,
                                      ResolveFunctionBreakpointCallback cb)
{
    bool isFull = IsFullPath(module);
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        ICorDebugModule *pModule = mdInfo.module.GetPtr();

        if (module != "") {
            ULONG32 nameLen;
            WCHAR szModuleName[mdNameLen] = {0};
            std::string modName;

            IfFailRet(pModule->GetName(_countof(szModuleName), &nameLen, szModuleName));

            if (isFull)
                modName = to_utf8(szModuleName);
            else
                modName = GetBasename(to_utf8(szModuleName));

            if (modName != module)
                continue;
        }

        if (SUCCEEDED(ResolveMethodInModule(mdInfo.module, funcname, cb)))
        {
            mdInfo.module->AddRef();
        }
    }

    return E_FAIL;
}


HRESULT Modules::ResolveFunctionInModule(ICorDebugModule *pModule,
                                         const std::string &module,
                                         std::string &funcname,
                                         ResolveFunctionBreakpointCallback cb)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;

    if (module != "")
    {
        ULONG32 len;
        WCHAR szModuleName[mdNameLen] = {0};
        std::string modName;

        IfFailRet(pModule->GetName(_countof(szModuleName), &len, szModuleName));

        if (IsFullPath(module))
            modName = to_utf8(szModuleName);
        else
            modName = GetBasename(to_utf8(szModuleName));

        if (modName != module)
            return E_FAIL;
    }

    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
        return E_FAIL;

    IfFailRet(ResolveMethodInModule(pModule, funcname, cb));

    return S_OK;
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

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(GetSequencePointByILOffset(info_pair->second.managedPart.get(), methodToken, ilOffset, &sequencePoint));

    return S_OK;
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

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.managedPart->GetStepRangesFromIP(nOffset, methodToken, &ilStartOffset, &ilEndOffset));
    }

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

HRESULT Modules::GetModuleId(ICorDebugModule *pModule, std::string &id)
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

// Fill m_asyncMethodsSteppingInfo by data from module. Called on callback during module load.
// [in] pModule - object that represents the CLR module;
// [in] managedPart - object that represents debugger's managed part with preloaded PDB for pModule.
HRESULT Modules::FillAsyncMethodsSteppingInfo(ICorDebugModule *pModule, ManagedPart *managedPart)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::vector<ManagedPart::AsyncAwaitInfoBlock> AsyncAwaitInfo;
    IfFailRet(managedPart->GetAsyncMethodsSteppingInfo(AsyncAwaitInfo));

    const std::lock_guard<std::mutex> lock(m_asyncMethodsSteppingInfoMutex);

    for (const auto &entry : AsyncAwaitInfo)
    {
        mdMethodDef realToken = mdMethodDefNil + entry.token;
        std::pair<CORDB_ADDRESS, mdMethodDef> newKey = std::make_pair(modAddress, realToken);
        m_asyncMethodsSteppingInfo[newKey].catch_handler_offset = entry.catch_handler_offset; // same for all awaits in async method
        m_asyncMethodsSteppingInfo[newKey].awaits.emplace_back(entry.yield_offset, entry.resume_offset);

        std::vector<ManagedPart::SequencePoint> points;
        IfFailRet(managedPart->GetSequencePoints(realToken, points));
        for (auto it = points.rbegin(); it != points.rend(); ++it)
        {
            if (it->startLine == 0 || it->startLine == ManagedPart::HiddenLine)
                continue;
            
            m_asyncMethodsSteppingInfo[newKey].lastIlOffset = it->offset;
            break;
        }
    }

    return S_OK;
}

// Check if method have await block. In this way we detect async method with awaits.
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
bool Modules::IsMethodHaveAwait(CORDB_ADDRESS modAddress, mdMethodDef methodToken)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodsSteppingInfoMutex);

    auto searchAsyncInfo = m_asyncMethodsSteppingInfo.find(std::make_pair(modAddress, methodToken));
    return searchAsyncInfo != m_asyncMethodsSteppingInfo.end();
}

// Find await block after IL offset in particular async method and return await info, if present.
// In case of async stepping, we need await info from PDB in order to setup breakpoints in proper places (yield and resume offsets).
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
// [in] ipOffset - IL offset;
// [out] awaitInfo - result, next await info.
bool Modules::FindNextAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 ipOffset, AwaitInfo **awaitInfo)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodsSteppingInfoMutex);

    auto searchAsyncInfo = m_asyncMethodsSteppingInfo.find(std::make_pair(modAddress, methodToken));
    if (searchAsyncInfo == m_asyncMethodsSteppingInfo.end())
        return false;

    for (auto &await : searchAsyncInfo->second.awaits)
    {
        if (ipOffset <= await.yield_offset)
        {
            if (awaitInfo)
                *awaitInfo = &await;
            return true;
        }
        // Stop search, if IP inside 'await' routine.
        else if (ipOffset < await.resume_offset)
        {
            break;
        }
    }

    return false;
}

// Find last IL offset for user code in async method, if present.
// In case of step-in and step-over we must detect last user code line in order to "emulate"
// step-out (DebuggerOfWaitCompletion magic) instead.
// [in] modAddress - module address;
// [in] methodToken - method token (from module with address modAddress).
// [out] lastIlOffset - result, IL offset for last user code line in async method.
bool Modules::FindLastIlOffsetAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 &lastIlOffset)
{
    const std::lock_guard<std::mutex> lock(m_asyncMethodsSteppingInfoMutex);

    auto searchAsyncInfo = m_asyncMethodsSteppingInfo.find(std::make_pair(modAddress, methodToken));
    if (searchAsyncInfo == m_asyncMethodsSteppingInfo.end())
        return false;

    lastIlOffset = searchAsyncInfo->second.lastIlOffset;
    return true;
}

HRESULT Modules::TryLoadModuleSymbols(
    ICorDebugModule *pModule,
    Module &module,
    bool needJMC
)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    module.path = GetModuleFileName(pModule);
    module.name = GetFileName(module.path);

    std::unique_ptr<ManagedPart> managedPart(new ManagedPart());

    managedPart->LoadSymbols(pMDImport, pModule);
    module.symbolStatus = managedPart->SymbolsLoaded() ? SymbolsLoaded : SymbolsNotFound;

    if (needJMC && module.symbolStatus == SymbolsLoaded)
    {
        // https://docs.microsoft.com/en-us/visualstudio/debugger/just-my-code
        // The .NET debugger considers optimized binaries and non-loaded .pdb files to be non-user code.
        // Three compiler attributes also affect what the .NET debugger considers to be user code:
        // * DebuggerNonUserCodeAttribute tells the debugger that the code it's applied to isn't user code.
        // * DebuggerHiddenAttribute hides the code from the debugger, even if Just My Code is turned off.
        // * DebuggerStepThroughAttribute tells the debugger to step through the code it's applied to, rather than step into the code.
        // The .NET debugger considers all other code to be user code.
        ToRelease<ICorDebugModule2> pModule2;
        if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
        {
            pModule2->SetJMCStatus(true, 0, nullptr);
            SetJMCFromAttributes(pModule, managedPart.get());
        }
    }

    if (module.symbolStatus == SymbolsLoaded)
    {
        if (FAILED(FillSourcesCodeLinesForModule(pMDImport, managedPart.get())))
            LOGE("Could not load source lines related info from PDB file. Could produce failures during breakpoint's source path resolve in future.");
        if (FAILED(FillAsyncMethodsSteppingInfo(pModule, managedPart.get())))
            LOGE("Could not load async methods related info from PDB file. Could produce failures during stepping in async methods in future.");
    }

    IfFailRet(GetModuleId(pModule, module.id));

    CORDB_ADDRESS baseAddress;
    ULONG32 size;
    IfFailRet(pModule->GetBaseAddress(&baseAddress));
    IfFailRet(pModule->GetSize(&size));
    module.baseAddress = baseAddress;
    module.size = size;

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        pModule->AddRef();
        ModuleInfo mdInfo { std::move(managedPart), pModule };
        m_modulesInfo.insert(std::make_pair(baseAddress, std::move(mdInfo)));
    }

    return S_OK;
}

HRESULT Modules::GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    WCHAR wParamName[mdNameLen] = W("\0");

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.managedPart->GetNamedLocalVariableAndScope(pILFrame, methodToken, localIndex, wParamName, _countof(wParamName), ppValue, pIlStart, pIlEnd));
    }

    paramName = to_utf8(wParamName);

    return S_OK;
}

HRESULT Modules::GetModuleWithName(const std::string &name, ICorDebugModule **ppModule)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        std::string path = GetModuleFileName(mdInfo.module);

        if (GetFileName(path) == name)
        {
            mdInfo.module->AddRef();
            *ppModule = mdInfo.module;
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT Modules::GetNextSequencePointInMethod(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG32 ilOffset,
    Modules::SequencePoint &sequencePoint)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(GetSequencePointByILOffset(info_pair->second.managedPart.get(), methodToken, ilOffset, &sequencePoint));

    return S_OK;
}

HRESULT Modules::GetSequencePointByILOffset(
    ManagedPart *managedPart,
    mdMethodDef methodToken,
    ULONG32 &ilOffset,
    SequencePoint *sequencePoint)
{
    ManagedPart::SequencePoint symSequencePoint;

    if (FAILED(managedPart->GetSequencePointByILOffset(methodToken, ilOffset, &symSequencePoint))) {
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

HRESULT Modules::ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb)
{
    HRESULT Status;
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        IfFailRet(cb(mdInfo.module));
    }
    return S_OK;
}

HRESULT Modules::FillSourcesCodeLinesForModule(IMetaDataImport *pMDImport, ManagedPart *managedPart)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    ULONG numTypedefs = 0;
    HCORENUM fEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMDImport->EnumTypeDefs(&fEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        ULONG numMethods = 0;
        HCORENUM fEnum = NULL;
        mdMethodDef methodDef;
        while(SUCCEEDED(pMDImport->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
        {
            std::vector<ManagedPart::SequencePoint> points;
            if (FAILED(managedPart->GetSequencePoints(methodDef, points)))
                continue;

            for (auto &point : points)
            {
                if (point.startLine == ManagedPart::HiddenLine)
                    continue;

                std::string fullPath = to_utf8(point.document);

#ifdef WIN32
                HRESULT Status = S_OK;
                std::string initialFullPath = fullPath;
                IfFailRet(ManagedPart::StringToUpper(fullPath));
                m_sourceCaseSensitiveFullPaths[fullPath] = initialFullPath;
#endif

                auto &codeLinesFullPath = m_sourcesCodeLines[fullPath];
                for (int i = point.startLine; i <= point.endLine; i++)
                    codeLinesFullPath[i] = std::make_tuple(point.startLine, point.endLine);

                m_sourcesFullPaths[GetFileName(fullPath)].insert(std::move(fullPath));
            }
        }
        pMDImport->CloseEnum(fEnum);
    }
    pMDImport->CloseEnum(fEnum);

    return S_OK;
}

HRESULT Modules::ResolveRelativeSourceFileName(std::string &filename)
{
    // IMPORTANT! Caller should care about m_sourcesInfoMutex.

    auto searchByFileName = m_sourcesFullPaths.find(GetFileName(filename));
    if (searchByFileName == m_sourcesFullPaths.end())
        return E_FAIL;

    auto const& possiblePaths = searchByFileName->second;
    std::string result = filename;

    // Care about all "./" and "../" first.
    std::list<std::string> pathDirs;
    std::size_t i;
    while ((i = result.find_first_of("/\\")) != std::string::npos)
    {
        std::string pathElement = result.substr(0, i);
        if (pathElement == "..")
        {
            if (!pathDirs.empty())
                pathDirs.pop_front();
        }
        else if (pathElement != ".")
            pathDirs.push_front(pathElement);

        result = result.substr(i + 1);
    }
    for (const auto &dir : pathDirs)
    {
        result = dir + '/' + result;
    }

    // The problem is - we could have several assemblies that could have same source file name with different path's root.
    // We don't really have a lot of options here, so, we assume, that all possible sources paths have same root and just find the shortest.
    if (result == GetFileName(result))
    {
        auto it = std::min_element(possiblePaths.begin(), possiblePaths.end(),
                        [](const std::string a, const std::string b){ return a.size() < b.size(); } );

        filename = it == possiblePaths.end() ? result : *it;
        return S_OK;
    }

    std::list<std::string> possibleResults;
    for (auto const& path : possiblePaths)
    {
        if (result.size() > path.size())
            continue;

        // Note, since assemblies could be built in different OSes, we could have different delimiters in source files paths.
        auto BinaryPredicate = [](const char& a, const char& b)
        {
            if ((a == '/' || a == '\\') && (b == '/' || b == '\\')) return true;
            return a == b;
        };

        // since C++17
        //if (std::equal(result.begin(), result.end(), path.end() - result.size(), BinaryPredicate))
        //    possibleResults.push_back(path);
        auto first1 = result.begin();
        auto last1 = result.end();
        auto first2 = path.end() - result.size();
        auto equal = [&]()
        {
            for (; first1 != last1; ++first1, ++first2)
            {
                if (!BinaryPredicate(*first1, *first2))
                    return false;
            }
            return true;
        };
        if (equal())
            possibleResults.push_back(path);
    }
    // The problem is - we could have several assemblies that could have sources with same relative paths with different path's root.
    // We don't really have a lot of options here, so, we assume, that all possible sources paths have same root and just find the shortest.
    if (!possibleResults.empty())
    {
        filename = possibleResults.front();
        for (const auto& path : possibleResults)
        {
            if (filename.length() > path.length())
                filename = path;
        }
        return S_OK;
    }

    return E_FAIL;
}

HRESULT Modules::ResolveBreakpointFileAndLine(std::string &filename, int &linenum, int &endLine)
{
    HRESULT Status;
#ifdef WIN32
    IfFailRet(ManagedPart::StringToUpper(filename));
#endif

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    auto searchByFullPath = m_sourcesCodeLines.find(filename);
    if (searchByFullPath == m_sourcesCodeLines.end())
    {
        // Check for absolute path.
#ifdef WIN32
        if (filename[1] == ':' && (filename[2] == '/' || filename[2] == '\\'))
#else
        if (filename[0] == '/')
#endif
        {
            return E_FAIL;
        }

        IfFailRet(ResolveRelativeSourceFileName(filename));

        searchByFullPath = m_sourcesCodeLines.find(filename);
        if (searchByFullPath == m_sourcesCodeLines.end())
            return E_FAIL;
    }

#ifdef WIN32
    // get proper case sensitive full path from module
    auto searchCaseSensitiveFullPath = m_sourceCaseSensitiveFullPaths.find(filename);
    if (searchCaseSensitiveFullPath == m_sourceCaseSensitiveFullPaths.end())
        return E_FAIL;
    filename = searchCaseSensitiveFullPath->second;
#endif

    auto &codeLines = searchByFullPath->second;

    auto resolvedLine = codeLines.lower_bound(linenum);
    if (resolvedLine == codeLines.end())
        return E_FAIL;

    linenum = std::get<0>(resolvedLine->second);
    endLine = std::get<1>(resolvedLine->second);

    return S_OK;
}


HRESULT GetModuleName(ICorDebugThread *pThread, std::string &module)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));

    if (pFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*)&pMDImport));

    WCHAR mdName[mdNameLen];
    ULONG nameLen;
    IfFailRet(pMDImport->GetScopeProps(mdName, _countof(mdName), &nameLen, nullptr));
    module = to_utf8(mdName);

    return S_OK;
}


void Modules::FindFileNames(string_view pattern, unsigned limit, std::function<void(const char *)> cb)
{
#ifdef WIN32
    HRESULT Status = S_OK;
    std::string uppercase {pattern};
    if (FAILED(ManagedPart::StringToUpper(uppercase)))
        return;
    pattern = uppercase;
#endif

    auto check = [&](const std::string& str)
    {
        if (limit == 0)
            return false;

        auto pos = str.find(pattern);
        if (pos != std::string::npos && (pos == 0 || str[pos-1] == '/' || str[pos-1] == '\\'))
        {
            limit--;
#ifndef _WIN32
            cb(str.c_str());
#else
            auto it = m_sourceCaseSensitiveFullPaths.find(str);
            cb (it != m_sourceCaseSensitiveFullPaths.end() ? it->second.c_str() : str.c_str());
#endif
        }

        return true;
    };

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);
    for (const auto& pair : m_sourcesFullPaths)
    {
        LOGD("first '%s'", pair.first.c_str());
        if (!check(pair.first))
            return;

        for (const std::string& str : pair.second)
        {
            LOGD("second '%s'", str.c_str());
            if (!check(str))
                return;
        }
    }
}

void Modules::FindFunctions(string_view pattern, unsigned limit, std::function<void(const char *)> cb)
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
        HRESULT Status = ForEachMethod(modpair.second.module, functor);
        if (FAILED(Status))
            break;
    }
}

} // namespace netcoredbg
