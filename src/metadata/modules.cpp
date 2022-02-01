// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/modules.h"

#include <sstream>
#include <vector>
#include <map>
#include <iomanip>
#include <algorithm>

#include "managed/interop.h"
#include "utils/platform.h"
#include "utils/utf.h"
#include "metadata/typeprinter.h"
#include "utils/filesystem.h"

namespace netcoredbg
{

namespace
{

    struct file_methods_data_t
    {
        BSTR document;
        int32_t methodNum;
        method_input_data_t *methodsData;

        file_methods_data_t() = delete;
        file_methods_data_t(const file_methods_data_t&) = delete;
        file_methods_data_t& operator=(const file_methods_data_t&) = delete;
        file_methods_data_t(file_methods_data_t&&) = delete;
        file_methods_data_t& operator=(file_methods_data_t&&) = delete;
    };

    struct module_methods_data_t
    {
        int32_t fileNum;
        file_methods_data_t *moduleMethodsData;

        module_methods_data_t() = delete;
        module_methods_data_t(const module_methods_data_t&) = delete;
        module_methods_data_t& operator=(const module_methods_data_t&) = delete;
        module_methods_data_t(module_methods_data_t&&) = delete;
        module_methods_data_t& operator=(module_methods_data_t&&) = delete;
    };

    struct module_methods_data_t_deleter
    {
        void operator()(module_methods_data_t *p) const
        {
            if (p->moduleMethodsData)
            {
                for (int32_t i = 0; i < p->fileNum; i++)
                {
                    if (p->moduleMethodsData[i].document)
                        Interop::SysFreeString(p->moduleMethodsData[i].document);
                    if (p->moduleMethodsData[i].methodsData)
                        Interop::CoTaskMemFree(p->moduleMethodsData[i].methodsData);
                }
                Interop::CoTaskMemFree(p->moduleMethodsData);
            }
            Interop::CoTaskMemFree(p);
        }
    };

    // Note, we use std::map since we need container that will not invalidate iterators on add new elements.
    void AddMethodData(/*in,out*/ std::map<size_t, std::set<method_input_data_t>> &methodData,
                       /*in,out*/ std::unordered_map<method_data_t, std::vector<mdMethodDef>, method_data_t_hash> &multiMethodBpData,
                       const method_input_data_t &entry,
                       const size_t nestedLevel)
    {
        // if we here, we need at least one nested level for sure
        if (methodData.empty())
        {
            methodData.emplace(std::make_pair(0, std::set<method_input_data_t>{entry}));
            return;
        }
        assert(nestedLevel <= methodData.size()); // could be increased only at 1 per recursive call
        if (nestedLevel == methodData.size())
        {
            methodData.emplace(std::make_pair(nestedLevel, std::set<method_input_data_t>{entry}));
            return;
        }

        // same data that was already added, but with different method token (constructors case)
        auto find = methodData[nestedLevel].find(entry);
        if (find != methodData[nestedLevel].end())
        {
            method_data_t key(find->methodDef, entry.startLine, entry.endLine);
            auto find_multi = multiMethodBpData.find(key);
            if (find_multi == multiMethodBpData.end())
                multiMethodBpData.emplace(std::make_pair(key, std::vector<mdMethodDef>{entry.methodDef}));
            else
                find_multi->second.emplace_back(entry.methodDef);

            return;
        }

        auto it = methodData[nestedLevel].lower_bound(entry);
        if (it != methodData[nestedLevel].end() && entry.NestedInto(*it))
        {
            AddMethodData(methodData, multiMethodBpData, entry, nestedLevel + 1);
            return;
        }

        // case with only one element on nested level, NestedInto() was already called and entry checked
        if (it == methodData[nestedLevel].begin())
        {
            methodData[nestedLevel].emplace(entry);
            return;
        }

        // move all previously added nested for new entry elements to level above
        do
        {
            it = std::prev(it);

            if ((*it).NestedInto(entry))
            {
                method_input_data_t tmp = *it;
                it = methodData[nestedLevel].erase(it);
                AddMethodData(methodData, multiMethodBpData, tmp, nestedLevel + 1);
            }
            else
                break;
        }
        while(it != methodData[nestedLevel].begin());

        methodData[nestedLevel].emplace(entry);
    }


    bool GetMethodTokensByLinuNumber(const std::vector<std::vector<method_data_t>> &methodBpData,
                                     const std::unordered_map<method_data_t, std::vector<mdMethodDef>, method_data_t_hash> &multiMethodBpData,
                                     /*in,out*/ uint32_t &lineNum,
                                     /*out*/ std::vector<mdMethodDef> &Tokens,
                                     /*out*/ mdMethodDef &closestNestedToken)
    {
        const method_data_t *result = nullptr;
        closestNestedToken = 0;

        for (auto it = methodBpData.cbegin(); it != methodBpData.cend(); ++it)
        {
            auto lower = std::lower_bound((*it).cbegin(), (*it).cend(), lineNum);
            if (lower == (*it).cend())
                break; // point behind last method for this nested level

            // case with first line of method, for example:
            // void Method(){ 
            //            void Method(){ void Method(){...  <- breakpoint at this line
            if (lineNum == (*lower).startLine)
            {
                // At this point we can't check this case, let managed part decide (since it see Columns):
                // void Method() {
                // ... code ...; void Method() {     <- breakpoint at this line
                //  };
                if (result)
                    closestNestedToken = (*lower).methodDef;
                else
                    result = &(*lower);

                break;
            }
            else if (lineNum > (*lower).startLine && (*lower).endLine >= lineNum)
            {
                result = &(*lower);
                continue; // need check nested level (if available)
            }
            // out of first level methods lines - forced move line to first method below, for example:
            //  <-- breakpoint at line without code (out of any methods)
            // void Method() {...}
            else if (it == methodBpData.cbegin() && lineNum < (*lower).startLine)
            {
                lineNum = (*lower).startLine;
                result = &(*lower);
                break;
            }
            // result was found on previous cycle, check for closest nested method
            // need it in case of breakpoint setuped at lines without code and before nested method, for example:
            // {
            //  <-- breakpoint at line without code (inside method)
            //     void Method() {...}
            // }
            else if (result && lineNum <= (*lower).startLine && (*lower).endLine <= result->endLine)
            {
                closestNestedToken = (*lower).methodDef;
                break;
            }
            else
                break;
        }

        if (result)
        {
            auto find = multiMethodBpData.find(*result);
            if (find != multiMethodBpData.end()) // only constructors segments could be part of multiple methods
            {
                Tokens.resize((*find).second.size());
                std::copy(find->second.begin(), find->second.end(), Tokens.begin());
            }
            Tokens.emplace_back(result->methodDef);
        }
        
        return !!result;
    }

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

} // unnamed namespace

Modules::ModuleInfo::~ModuleInfo() noexcept
{
    if (m_symbolReaderHandle)
        Interop::DisposeSymbols(m_symbolReaderHandle);
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

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
        return E_FAIL;

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

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    ModuleInfo &mdInfo = info_pair->second;
    return GetSequencePointByILOffset(mdInfo.m_symbolReaderHandle, methodToken, ilOffset, &sequencePoint);
}

HRESULT Modules::GetFrameILAndNextUserCodeILOffset(
    ICorDebugFrame *pFrame,
    ULONG32 &ilOffset,
    ULONG32 &ilCloseOffset,
    bool *noUserCodeFound)
{
    HRESULT Status;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    return GetNextSequencePointInMethod(pModule, methodToken, ilOffset, ilCloseOffset, noUserCodeFound);
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

        ModuleInfo &mdInfo = info_pair->second;
        IfFailRet(Interop::GetStepRangesFromIP(mdInfo.m_symbolReaderHandle, nOffset, methodToken, &ilStartOffset, &ilEndOffset));
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

// Fill m_asyncMethodsSteppingInfo by data from module. Called on callback during module load.
// [in] pModule - object that represents the CLR module;
// [in] pSymbolReaderHandle - pointer to managed part GCHandle with preloaded PDB.
HRESULT Modules::FillAsyncMethodsSteppingInfo(ICorDebugModule *pModule, PVOID pSymbolReaderHandle)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::vector<Interop::AsyncAwaitInfoBlock> AsyncAwaitInfo;
    IfFailRet(Interop::GetAsyncMethodsSteppingInfo(pSymbolReaderHandle, AsyncAwaitInfo));

    const std::lock_guard<std::mutex> lock(m_asyncMethodsSteppingInfoMutex);

    for (const auto &entry : AsyncAwaitInfo)
    {
        mdMethodDef realToken = mdMethodDefNil + entry.token;
        std::pair<CORDB_ADDRESS, mdMethodDef> newKey = std::make_pair(modAddress, realToken);
        m_asyncMethodsSteppingInfo[newKey].awaits.emplace_back(entry.yield_offset, entry.resume_offset);

        IfFailRet(Interop::GetMethodLastIlOffset(pSymbolReaderHandle, realToken, &m_asyncMethodsSteppingInfo[newKey].lastIlOffset));
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

HRESULT Modules::TryLoadModuleSymbols(ICorDebugModule *pModule, Module &module, bool needJMC, std::string &outputText)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    module.path = GetModuleFileName(pModule);
    module.name = GetFileName(module.path);

    PVOID pSymbolReaderHandle = nullptr;
    Interop::LoadSymbols(pMDImport, pModule, &pSymbolReaderHandle);
    module.symbolStatus = pSymbolReaderHandle != nullptr ? SymbolsLoaded : SymbolsNotFound;

    if (module.symbolStatus == SymbolsLoaded)
    {
        ToRelease<ICorDebugModule2> pModule2;
        if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
        {
            if (!needJMC)
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
                    DisableJMCByAttributes(pModule, pSymbolReaderHandle);
            }
            else if (Status == CORDBG_E_CANT_SET_TO_JMC)
            {
                if (needJMC)
                    outputText = "You are debugging a Release build of " + module.name + ". Using Just My Code with Release builds using compiler optimizations results in a degraded debugging experience (e.g. breakpoints will not be hit).";
                else
                    outputText = "You are debugging a Release build of " + module.name + ". Without Just My Code Release builds try not to use compiler optimizations, but in some cases (e.g. attach) this still results in a degraded debugging experience (e.g. breakpoints will not be hit).";
            }
        }

        if (FAILED(FillSourcesCodeLinesForModule(pModule, pMDImport, pSymbolReaderHandle)))
            LOGE("Could not load source lines related info from PDB file. Could produce failures during breakpoint's source path resolve in future.");

        if (FAILED(FillAsyncMethodsSteppingInfo(pModule, pSymbolReaderHandle)))
            LOGE("Could not load async methods related info from PDB file. Could produce failures during stepping in async methods in future.");
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
    m_modulesInfo.insert(std::make_pair(baseAddress, std::move(mdInfo)));

    return S_OK;
}

HRESULT Modules::GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG localIndex,
    WSTRING &localName,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    WCHAR wLocalName[mdNameLen] = W("\0");

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
        {
            return E_FAIL;
        }

        ModuleInfo &mdInfo = info_pair->second;
        IfFailRet(Interop::GetNamedLocalVariableAndScope(mdInfo.m_symbolReaderHandle, methodToken, localIndex, wLocalName, _countof(wLocalName), pIlStart, pIlEnd));
    }

    localName = wLocalName;

    return S_OK;
}

HRESULT Modules::GetHoistedLocalScopes(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    PVOID *data,
    int32_t &hoistedLocalScopesCount)
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

    ModuleInfo &mdInfo = info_pair->second;
    IfFailRet(Interop::GetHoistedLocalScopes(mdInfo.m_symbolReaderHandle, methodToken, data, hoistedLocalScopesCount));
    return S_OK;
}

HRESULT Modules::GetModuleWithName(const std::string &name, ICorDebugModule **ppModule)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        std::string path = GetModuleFileName(mdInfo.m_iCorModule);

        if (GetFileName(path) == name)
        {
            mdInfo.m_iCorModule->AddRef();
            *ppModule = mdInfo.m_iCorModule;
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT Modules::GetNextSequencePointInMethod(
    ICorDebugModule *pModule,
    mdMethodDef methodToken,
    ULONG32 ilOffset,
    ULONG32 &ilCloseOffset,
    bool *noUserCodeFound)
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

    ModuleInfo &mdInfo = info_pair->second;
    return Interop::GetNextSequencePointByILOffset(mdInfo.m_symbolReaderHandle, methodToken, ilOffset, ilCloseOffset, noUserCodeFound);
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
    ULONG32 ilOffset,
    Modules::SequencePoint &sequencePoint)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    ModuleInfo &mdInfo = info_pair->second;
    return GetSequencePointByILOffset(mdInfo.m_symbolReaderHandle, methodToken, ilOffset, &sequencePoint);
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

HRESULT Modules::FillSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    HRESULT Status;
    // Note, we need 2 arrays of tokens - for normal methods and constructors (.ctor/.cctor, that could have segmented code).
    std::vector<int32_t> constrTokens;
    std::vector<int32_t> normalTokens;

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
            WCHAR funcName[mdNameLen];
            ULONG funcNameLen;
            if (FAILED(pMDImport->GetMethodProps(methodDef, nullptr, funcName, _countof(funcName), &funcNameLen,
                                                 nullptr, nullptr, nullptr, nullptr, nullptr)))
            {
                continue;
            }

            if (str_equal(funcName, W(".ctor")) || str_equal(funcName, W(".cctor")))
                constrTokens.emplace_back(methodDef);
            else
                normalTokens.emplace_back(methodDef);
        }
        pMDImport->CloseEnum(fEnum);
    }
    pMDImport->CloseEnum(fEnum);

    if (constrTokens.size() > std::numeric_limits<int32_t>::max() || normalTokens.size() > std::numeric_limits<int32_t>::max())
    {
        LOGE("Too big token arrays.");
        return E_FAIL;
    }

    PVOID data = nullptr;
    IfFailRet(Interop::GetModuleMethodsRanges(pSymbolReaderHandle, (int32_t)constrTokens.size(), constrTokens.data(), (int32_t)normalTokens.size(), normalTokens.data(), &data));
    if (data == nullptr)
        return S_OK;

    std::unique_ptr<module_methods_data_t, module_methods_data_t_deleter> inputData((module_methods_data_t*)data);
    constrTokens.clear();
    normalTokens.clear();

    // Usually, modules provide files with unique full paths for sources.
    m_sourceIndexToPath.reserve(m_sourceIndexToPath.size() + inputData->fileNum);
    m_sourcesMethodsData.reserve(m_sourcesMethodsData.size() + inputData->fileNum);
#ifdef WIN32
    m_sourceIndexToInitialFullPath.reserve(m_sourceIndexToInitialFullPath.size() + inputData->fileNum);
#endif

    for (int i = 0; i < inputData->fileNum; i++)
    {
        std::string fullPath = to_utf8(inputData->moduleMethodsData[i].document);
#ifdef WIN32
        std::string initialFullPath = fullPath;
        IfFailRet(Interop::StringToUpper(fullPath));
#endif
        auto findPathIndex = m_sourcePathToIndex.find(fullPath);
        unsigned fullPathIndex;
        if (findPathIndex == m_sourcePathToIndex.end())
        {
            fullPathIndex = (unsigned)m_sourceIndexToPath.size();
            m_sourcePathToIndex.emplace(std::make_pair(fullPath, fullPathIndex));
            m_sourceIndexToPath.emplace_back(fullPath);
#ifdef WIN32
            m_sourceIndexToInitialFullPath.emplace_back(initialFullPath);
#endif
            m_sourceNameToFullPathsIndexes[GetFileName(fullPath)].emplace(fullPathIndex);
            m_sourcesMethodsData.emplace_back(std::vector<FileMethodsData>{});
        }
        else
            fullPathIndex = findPathIndex->second;

        m_sourcesMethodsData[fullPathIndex].emplace_back(FileMethodsData{});
        auto &fileMethodsData = m_sourcesMethodsData[fullPathIndex].back();
        IfFailRet(pModule->GetBaseAddress(&fileMethodsData.modAddress));

        // Note, don't reorder input data, since it have almost ideal order for us.
        // For example, for Private.CoreLib (about 22000 methods) only 8 relocations were made.
        // In case default methods ordering will be dramatically changed, we could use data reordering,
        // for example based on this solution:
        //    struct compare {
        //        bool operator()(const method_input_data_t &lhs, const method_input_data_t &rhs) const
        //        { return lhs.endLine > rhs.endLine || (lhs.endLine == rhs.endLine && lhs.endColumn > rhs.endColumn); }
        //    };
        //    std::multiset<method_input_data_t, compare> orderedInputData;
        std::map<size_t, std::set<method_input_data_t>> inputMethodsData;
        for (int j = 0; j < inputData->moduleMethodsData[i].methodNum; j++)
        {
            AddMethodData(inputMethodsData, fileMethodsData.multiMethodsData, inputData->moduleMethodsData[i].methodsData[j], 0);
        }

        fileMethodsData.methodsData.resize(inputMethodsData.size());
        for (size_t i =  0; i < inputMethodsData.size(); i++)
        {
            fileMethodsData.methodsData[i].resize(inputMethodsData[i].size());
            std::copy(inputMethodsData[i].begin(), inputMethodsData[i].end(), fileMethodsData.methodsData[i].begin());
        }
        for (auto &data : fileMethodsData.multiMethodsData)
        {
            data.second.shrink_to_fit();
        }
    }

    m_sourcesMethodsData.shrink_to_fit();
    m_sourceIndexToPath.shrink_to_fit();
#ifdef WIN32
    m_sourceIndexToInitialFullPath.shrink_to_fit();
#endif

    return S_OK;
}

HRESULT Modules::ResolveRelativeSourceFileName(std::string &filename)
{
    // IMPORTANT! Caller should care about m_sourcesInfoMutex.
    auto findIndexesByFileName = m_sourceNameToFullPathsIndexes.find(GetFileName(filename));
    if (findIndexesByFileName == m_sourceNameToFullPathsIndexes.end())
        return E_FAIL;

    auto const &possiblePathsIndexes = findIndexesByFileName->second;
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
        auto it = std::min_element(possiblePathsIndexes.begin(), possiblePathsIndexes.end(),
                        [&](const unsigned a, const unsigned b){ return m_sourceIndexToPath[a].size() < m_sourceIndexToPath[b].size(); } );

        filename = it == possiblePathsIndexes.end() ? result : m_sourceIndexToPath[*it];
        return S_OK;
    }

    std::list<std::string> possibleResults;
    for (const auto pathIndex : possiblePathsIndexes)
    {
        if (result.size() > m_sourceIndexToPath[pathIndex].size())
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
        auto first2 = m_sourceIndexToPath[pathIndex].end() - result.size();
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
            possibleResults.push_back(m_sourceIndexToPath[pathIndex]);
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

HRESULT Modules::ResolveBreakpoint(/*in*/ CORDB_ADDRESS modAddress, /*in*/ std::string filename, /*out*/ unsigned &fullname_index,
                                   /*in*/ int sourceLine, /*out*/ std::vector<resolved_bp_t> &resolvedPoints)
{
    HRESULT Status;
#ifdef WIN32
    IfFailRet(Interop::StringToUpper(filename));
#endif

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    auto findIndex = m_sourcePathToIndex.find(filename);
    if (findIndex == m_sourcePathToIndex.end())
    {
        // Check for absolute path.
#ifdef WIN32
        // Check, if start from drive letter, for example "D:\" or "D:/".
        if (filename.size() > 2 && filename[1] == ':' && (filename[2] == '/' || filename[2] == '\\'))
#else
        if (filename[0] == '/')
#endif
        {
            return E_FAIL;
        }

        IfFailRet(ResolveRelativeSourceFileName(filename));

        findIndex = m_sourcePathToIndex.find(filename);
        if (findIndex == m_sourcePathToIndex.end())
            return E_FAIL;
    }

    fullname_index = findIndex->second;

    struct resolved_input_bp_t
    {
        int32_t startLine;
        int32_t endLine;
        uint32_t ilOffset;
        uint32_t methodToken;
    };

    struct resolved_input_bp_t_deleter
    {
        void operator()(resolved_input_bp_t *p) const
        {
            Interop::CoTaskMemFree(p);
        }
    };

    for (const auto &sourceData : m_sourcesMethodsData[findIndex->second])
    {
        if (modAddress && modAddress != sourceData.modAddress)
            continue;

        std::vector<mdMethodDef> Tokens;
        uint32_t correctedStartLine = sourceLine;
        mdMethodDef closestNestedToken = 0;
        if (!GetMethodTokensByLinuNumber(sourceData.methodsData, sourceData.multiMethodsData, correctedStartLine, Tokens, closestNestedToken))
            continue;
        // correctedStartLine - in case line not belong any methods, if possible, will be "moved" to first line of method below sourceLine.

        auto info_pair = m_modulesInfo.find(sourceData.modAddress);
        if (info_pair == m_modulesInfo.end())
            return E_FAIL; // we must have it, since we loaded data from it

        if (Tokens.size() > std::numeric_limits<int32_t>::max())
        {
            LOGE("Too big token arrays.");
            return E_FAIL;
        }

        PVOID data = nullptr;
        int32_t Count = 0;
        if (FAILED(Interop::ResolveBreakPoints(info_pair->second.m_symbolReaderHandle, (int32_t)Tokens.size(), Tokens.data(),
                                               correctedStartLine, closestNestedToken, Count, &data))
            || data == nullptr)
        {
            continue;
        }
        std::unique_ptr<resolved_input_bp_t, resolved_input_bp_t_deleter> inputData((resolved_input_bp_t*)data);

        for (int32_t i = 0; i < Count; i++)
        {
            info_pair->second.m_iCorModule->AddRef();
            resolvedPoints.emplace_back(resolved_bp_t(inputData.get()[i].startLine, inputData.get()[i].endLine, inputData.get()[i].ilOffset,
                                                      inputData.get()[i].methodToken, info_pair->second.m_iCorModule.GetPtr()));
        }
    }

    return S_OK;
}

HRESULT Modules::GetSourceFullPathByIndex(unsigned index, std::string &fullPath)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    if (m_sourceIndexToPath.size() <= index)
        return E_FAIL;

#ifndef _WIN32
    fullPath = m_sourceIndexToPath[index];
#else
    fullPath = m_sourceIndexToInitialFullPath[index];
#endif

    return S_OK;
}

HRESULT Modules::GetIndexBySourceFullPath(std::string fullPath, unsigned &index)
{
#ifdef WIN32
    HRESULT Status;
    IfFailRet(Interop::StringToUpper(fullPath));
#endif

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    auto findIndex = m_sourcePathToIndex.find(fullPath);
    if (findIndex == m_sourcePathToIndex.end())
        return E_FAIL;

    index = findIndex->second;
    return S_OK;
}

void Modules::FindFileNames(string_view pattern, unsigned limit, std::function<void(const char *)> cb)
{
#ifdef WIN32
    std::string uppercase {pattern};
    if (FAILED(Interop::StringToUpper(uppercase)))
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
            auto it = m_sourcePathToIndex.find(str);
            cb (it != m_sourcePathToIndex.end() ? m_sourceIndexToInitialFullPath[it->second].c_str() : str.c_str());
#endif
        }

        return true;
    };

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);
    for (const auto &pair : m_sourceNameToFullPathsIndexes)
    {
        LOGD("first '%s'", pair.first.c_str());
        if (!check(pair.first))
            return;

        for (const unsigned fileIndex : pair.second)
        {
            LOGD("second '%s'", m_sourceIndexToPath[fileIndex].c_str());
            if (!check(m_sourceIndexToPath[fileIndex]))
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

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    ModuleInfo &mdInfo = info_pair->second;
    return Interop::GetSource(mdInfo.m_symbolReaderHandle, sourcePath, (PVOID*)fileBuf, fileLen);
}

} // namespace netcoredbg
