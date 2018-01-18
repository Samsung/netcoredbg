// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <iomanip>

#include "symbolreader.h"
#include "cputil.h"
#include "platform.h"
#include "modules.h"

// JMC
bool ShouldLoadSymbolsForModule(const std::string &moduleName);
HRESULT SetJMCFromAttributes(ICorDebugModule *pModule, SymbolReader *symbolReader);

void Modules::CleanupAllModules()
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesInfo.clear();
}

std::string Modules::GetModuleFileName(ICorDebugModule *pModule)
{
    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;
    if (SUCCEEDED(pModule->GetName(_countof(name), &name_len, name)))
    {
        return to_utf8(name/*, name_len*/);
    }
    return std::string();
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

    WCHAR nameBuffer[MAX_LONGPATH];

    Status = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), filename.size() + 1, nameBuffer, MAX_LONGPATH);

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        CORDB_ADDRESS modAddress;
        IfFailRet(mdInfo.module->GetBaseAddress(&modAddress));
        if (FAILED(mdInfo.symbols->ResolveSequencePoint(nameBuffer, linenum, modAddress, &methodToken, &ilOffset)))
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

    WCHAR nameBuffer[MAX_LONGPATH];

    Status = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), filename.size() + 1, nameBuffer, MAX_LONGPATH);

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(info_pair->second.symbols->ResolveSequencePoint(nameBuffer, linenum, modAddress, &methodToken, &ilOffset));

    WCHAR wFilename[MAX_LONGPATH];
    ULONG resolvedLinenum;
    IfFailRet(info_pair->second.symbols->GetLineByILOffset(methodToken, ilOffset, &resolvedLinenum, wFilename, _countof(wFilename)));

    fullname = to_utf8(wFilename);

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

    for (auto &p : points)
    {
        if (p.offset < ilOffset)
        {
            nearestPoint = p;
            continue;
        }
        if (p.offset == ilOffset)
            nearestPoint = p;

        sequencePoint.startLine = nearestPoint.startLine;
        sequencePoint.endLine = nearestPoint.endLine;
        sequencePoint.startColumn = nearestPoint.startColumn;
        sequencePoint.endColumn = nearestPoint.endColumn;
        sequencePoint.offset = nearestPoint.offset;
        sequencePoint.document = to_utf8(name);
        return S_OK;
    }

    return E_FAIL;
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

    std::stringstream ss;
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
    std::string &id,
    std::string &name,
    bool &symbolsLoaded,
    CORDB_ADDRESS &baseAddress,
    ULONG32 &size)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    name = GetModuleFileName(pModule);

    std::unique_ptr<SymbolReader> symbolReader(new SymbolReader());

    if (ShouldLoadSymbolsForModule(name))
    {
        symbolReader->LoadSymbols(pMDImport, pModule);
        symbolsLoaded = symbolReader->SymbolsLoaded();
    }

    // JMC stuff
    ToRelease<ICorDebugModule2> pModule2;
    if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
    {
        pModule2->SetJMCStatus(symbolsLoaded, 0, nullptr);
        if (symbolsLoaded)
            SetJMCFromAttributes(pModule, symbolReader.get());
    }

    IfFailRet(GetModuleId(pModule, id));

    IfFailRet(pModule->GetBaseAddress(&baseAddress));
    IfFailRet(pModule->GetSize(&size));

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
