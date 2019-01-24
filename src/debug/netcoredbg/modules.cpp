// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "modules.h"

#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>

#include "symbolreader.h"
#include "platform.h"
#include "cputil.h"
#include "typeprinter.h"


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
    HRESULT Status;
    std::vector<std::string> splittedName = split_on_tokens(funcName, '.');
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

            std::vector<std::string> splittedFullName = split_on_tokens(typeName + "." + fullName, '.');

            // If we've found the target function
            if (IsTargetFunction(splittedFullName, splittedName))
            {
                if (FAILED(cb(pModule, mdMethod)))
                {
                    pMDImport->CloseEnum(fFuncEnum);
                    pMDImport->CloseEnum(fTypeEnum);

                    return E_FAIL;
                }
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

HRESULT Modules::GetLocationInAny(
    std::string filename,
    ULONG linenum,
    ULONG32 &ilOffset,
    mdMethodDef &methodToken,
    std::string &fullname,
    ICorDebugModule **ppModule)
{
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        CORDB_ADDRESS modAddress;
        IfFailRet(mdInfo.module->GetBaseAddress(&modAddress));
        if (FAILED(mdInfo.symbols->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset)))
            continue;

        WCHAR wFilename[MAX_LONGPATH];
        ULONG resolvedLinenum;
        if (FAILED(mdInfo.symbols->GetLineByILOffset(methodToken, ilOffset, &resolvedLinenum, wFilename, _countof(wFilename))))
            continue;

        fullname = to_utf8(wFilename);

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
    mdMethodDef &methodToken,
    std::string &fullname)
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

    IfFailRet(info_pair->second.symbols->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset));

    WCHAR wFilename[MAX_LONGPATH];
    ULONG resolvedLinenum;
    IfFailRet(info_pair->second.symbols->GetLineByILOffset(methodToken, ilOffset, &resolvedLinenum, wFilename, _countof(wFilename)));

    fullname = to_utf8(wFilename);

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

    WCHAR name[MAX_LONGPATH];

    std::vector<SymbolReader::SequencePoint> points;
    ULONG linenum;

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.symbols->GetLineByILOffset(methodToken, ilOffset, &linenum, name, _countof(name)));
        IfFailRet(info_pair->second.symbols->GetSequencePoints(methodToken, points));
    }

    if (points.empty())
        return E_FAIL;

    // TODO: Merge with similar code in SymbolReader.cs

    SymbolReader::SequencePoint &nearestPoint = points.front();
    bool found = false;

    for (auto &p : points)
    {
        if (found && p.offset > static_cast<int32_t>(ilOffset))
            break;

        if (p.startLine != SymbolReader::HiddenLine)
        {
            nearestPoint = p;
            found = true;
        }
    }

    sequencePoint.startLine = nearestPoint.startLine;
    sequencePoint.endLine = nearestPoint.endLine;
    sequencePoint.startColumn = nearestPoint.startColumn;
    sequencePoint.endColumn = nearestPoint.endColumn;
    sequencePoint.offset = nearestPoint.offset;
    sequencePoint.document = to_utf8(name);
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

        IfFailRet(info_pair->second.symbols->GetStepRangesFromIP(nOffset, methodToken, &ilStartOffset, &ilEndOffset));
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

HRESULT Modules::TryLoadModuleSymbols(
    ICorDebugModule *pModule,
    Module &module
)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    module.path = GetModuleFileName(pModule);
    module.name = GetFileName(module.path);

    std::unique_ptr<SymbolReader> symbolReader(new SymbolReader());

    if (ShouldLoadSymbolsForModule(module.path))
    {
        symbolReader->LoadSymbols(pMDImport, pModule);
        module.symbolStatus = symbolReader->SymbolsLoaded() ? SymbolsLoaded : SymbolsNotFound;
    }
    else
    {
        module.symbolStatus = SymbolsSkipped;
    }

    // JMC stuff
    ToRelease<ICorDebugModule2> pModule2;
    if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
    {
        pModule2->SetJMCStatus(module.symbolStatus == SymbolsLoaded, 0, nullptr);
        if (module.symbolStatus == SymbolsLoaded)
            SetJMCFromAttributes(pModule, symbolReader.get());
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
        ModuleInfo mdInfo { std::move(symbolReader), pModule };
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

        IfFailRet(info_pair->second.symbols->GetNamedLocalVariableAndScope(pILFrame, methodToken, localIndex, wParamName, _countof(wParamName), ppValue, pIlStart, pIlEnd));
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
