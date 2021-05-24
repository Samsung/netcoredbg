// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_line.h"
#include "debugger/breakpointutils.h"
#include "metadata/modules.h"
#include "utils/filesystem.h"
#include <unordered_set>
#include <algorithm>

namespace netcoredbg
{

void LineBreakpoints::ManagedLineBreakpoint::ToBreakpoint(Breakpoint &breakpoint, const std::string &fullname)
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsVerified();
    breakpoint.condition = this->condition;
    breakpoint.source = Source(fullname);
    breakpoint.line = this->linenum;
    breakpoint.endLine = this->endLine;
    breakpoint.hitCount = this->times;
}

void LineBreakpoints::DeleteAll()
{
    m_breakpointsMutex.lock();
    m_lineResolvedBreakpoints.clear();
    m_lineBreakpointMapping.clear();
    m_breakpointsMutex.unlock();
}

HRESULT LineBreakpoints::ManagedCallbackBreakpoint(IDebugger *debugger, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint)
{
    HRESULT Status;
    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    IfFailRet(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID *) &pFunctionBreakpoint));

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ULONG32 ilOffset;
    Modules::SequencePoint sp;
    IfFailRet(m_sharedModules->GetFrameILAndSequencePoint(pFrame, ilOffset, sp));

    unsigned filenameIndex;
    IfFailRet(m_sharedModules->GetIndexBySourceFullPath(sp.document, filenameIndex));

    auto breakpoints = m_lineResolvedBreakpoints.find(filenameIndex);
    if (breakpoints == m_lineResolvedBreakpoints.end())
        return E_FAIL;

    auto &breakpointsInSource = breakpoints->second;
    auto it = breakpointsInSource.find(sp.startLine);
    if (it == breakpointsInSource.end())
        return S_FALSE; // Stopped at break, but no breakpoints.

    std::list<ManagedLineBreakpoint> &bList = it->second;
    if (bList.empty())
        return S_FALSE; // Stopped at break, but no breakpoints.

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    // Same logic as provide vsdbg - only one breakpoint is active for one line, find first active in the list.
    for (auto &b : bList)
    {
        if (!b.enabled)
            continue;

        for (const auto &iCorFuncBreakpoint : b.iCorFuncBreakpoints)
        {
            if (FAILED(BreakpointUtils::IsSameFunctionBreakpoint(pFunctionBreakpoint, iCorFuncBreakpoint)) ||
                FAILED(BreakpointUtils::IsEnableByCondition(b.condition, debugger, pThread)))
                continue;

            ++b.times;
            b.ToBreakpoint(breakpoint, sp.document);
            return S_OK;
        }
    }

    return S_FALSE; // Stopped at break, but breakpoint not found.
}

HRESULT LineBreakpoints::ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &initialBreakpoints : m_lineBreakpointMapping)
    {
        for (auto &initialBreakpoint : initialBreakpoints.second)
        {
            if (initialBreakpoint.resolved_linenum)
                continue;

            ManagedLineBreakpoint bp;
            bp.id = initialBreakpoint.id;
            bp.module = initialBreakpoint.breakpoint.module;
            bp.enabled = initialBreakpoint.enabled;
            bp.linenum = initialBreakpoint.breakpoint.line;
            bp.endLine = initialBreakpoint.breakpoint.line;
            bp.condition = initialBreakpoint.breakpoint.condition;
            unsigned resolved_fullname_index = 0;

            if (FAILED(ResolveLineBreakpoint(pModule, bp, initialBreakpoints.first, resolved_fullname_index)))
                continue;

            std::string resolved_fullname;
            m_sharedModules->GetSourceFullPathByIndex(resolved_fullname_index, resolved_fullname);

            Breakpoint breakpoint;
            bp.ToBreakpoint(breakpoint, resolved_fullname);
            events.emplace_back(BreakpointChanged, breakpoint);

            initialBreakpoint.resolved_fullname_index = resolved_fullname_index;
            initialBreakpoint.resolved_linenum = bp.linenum;

            m_lineResolvedBreakpoints[resolved_fullname_index][initialBreakpoint.resolved_linenum].push_back(std::move(bp));
            EnableOneICorBreakpointForLine(m_lineResolvedBreakpoints[resolved_fullname_index][initialBreakpoint.resolved_linenum]);
        }
    }

    return S_OK;
}

HRESULT LineBreakpoints::EnableOneICorBreakpointForLine(std::list<ManagedLineBreakpoint> &bList)
{
    // Same logic as provide vsdbg - only one breakpoint is active for one line.
    BOOL needEnable = TRUE;
    HRESULT Status = S_OK;
    for (auto it = bList.begin(); it != bList.end(); ++it)
    {
        if ((*it).iCorFuncBreakpoints.empty())
            continue;

        if ((*it).enabled)
        {
            for (const auto &iCorFuncBreakpoint : (*it).iCorFuncBreakpoints)
            {
                HRESULT ret = iCorFuncBreakpoint->Activate(needEnable);
                Status = FAILED(ret) ? ret : Status;
            }
            needEnable = FALSE;
        }
        else
        {
            for (const auto &iCorFuncBreakpoint : (*it).iCorFuncBreakpoints)
            {
                iCorFuncBreakpoint->Activate(FALSE);
            }
        }
    }
    return Status;
}

// [in] pModule - optional, provide filter by module during resolve
// [in,out] bp - breakpoint data for resolve
HRESULT LineBreakpoints::ResolveLineBreakpoint(ICorDebugModule *pModule, ManagedLineBreakpoint &bp, const std::string &bp_fullname, unsigned &bp_fullname_index)
{
    if (bp_fullname.empty() || bp.linenum <= 0 || bp.endLine <= 0)
        return E_INVALIDARG;

    HRESULT Status;
    CORDB_ADDRESS modAddress = 0;

    if (!bp.module.empty() && pModule)
    {
        IfFailRet(Modules::IsModuleHaveSameName(pModule, bp.module, IsFullPath(bp.module)));
        if (Status == S_FALSE)
            return E_FAIL;
    }
    else if (!bp.module.empty())
    {
        bool isFullPath = IsFullPath(bp.module);
        m_sharedModules->ForEachModule([&bp, &modAddress, &isFullPath, &Status](ICorDebugModule *pModule) -> HRESULT
        {
            IfFailRet(Modules::IsModuleHaveSameName(pModule, bp.module, isFullPath));
            if (Status == S_FALSE)
                return S_FALSE;

            IfFailRet(pModule->GetBaseAddress(&modAddress));

            return E_ABORT; // Fast exit from cycle.
        });

        if (!modAddress)
            return E_FAIL;
    }
    else if (pModule) // Filter data from only one module during resolve, if need.
        IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::string fullname = bp_fullname;
    std::vector<Modules::resolved_bp_t> resolvedPoints;
    unsigned fullname_index = 0;

    IfFailRet(m_sharedModules->ResolveBreakpoint(modAddress, bp_fullname, fullname_index, bp.linenum, resolvedPoints));
    if (resolvedPoints.empty())
        return E_FAIL;

    modAddress = 0;
    CORDB_ADDRESS modAddressTrack = 0;
    for (const auto &resolvedBP : resolvedPoints)
    {
        // Note, we might have situation with same source path in different modules.
        // VSCode/MI protocols and internal debugger routine don't support this case.
        IfFailRet(resolvedBP.iCorModule->GetBaseAddress(&modAddressTrack));
        if (modAddress && modAddress != modAddressTrack)
        {
            LOGW("During breakpoint resolve, multiple modules with same source file path was detected.");
            LOGW("File name: %s", fullname.c_str());
            LOGW("Breakpoint activated in module: %s", Modules::GetModuleFileName(resolvedPoints[0].iCorModule).c_str());
            LOGW("Ignored module: %s", Modules::GetModuleFileName(resolvedBP.iCorModule).c_str());
            continue;
        }

        IfFailRet(BreakpointUtils::SkipBreakpoint(resolvedBP.iCorModule, resolvedBP.methodToken, m_justMyCode));
        if (Status == S_OK) // S_FALSE - don't skip breakpoint
            continue;

        modAddress = modAddressTrack;

        ToRelease<ICorDebugFunction> pFunc;
        IfFailRet(resolvedBP.iCorModule->GetFunctionFromToken(resolvedBP.methodToken, &pFunc));
        ToRelease<ICorDebugCode> pCode;
        IfFailRet(pFunc->GetILCode(&pCode));

        ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
        IfFailRet(pCode->CreateBreakpoint(resolvedBP.ilOffset, &iCorFuncBreakpoint));
        IfFailRet(iCorFuncBreakpoint->Activate(bp.enabled ? TRUE : FALSE));

        bp.iCorFuncBreakpoints.emplace_back(iCorFuncBreakpoint.Detach());
    }

    if (modAddress == 0)
        return E_FAIL;

    // No reason leave extra space here, since breakpoint could be setup for 1 module only (no more breakpoints will be added).
    bp.iCorFuncBreakpoints.shrink_to_fit();

    // same for multiple breakpoint resolve for one module
    bp.linenum = resolvedPoints[0].startLine;
    bp.endLine = resolvedPoints[0].endLine;
    bp_fullname_index = fullname_index;

    return S_OK;
}

HRESULT LineBreakpoints::SetLineBreakpoints(ICorDebugProcess *pProcess, const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints,
                                            std::vector<Breakpoint> &breakpoints, std::function<uint32_t()> getId)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto RemoveResolvedByInitialBreakpoint = [&](ManagedLineBreakpointMapping &initialBreakpoint)
    {
        if (!initialBreakpoint.resolved_linenum)
            return S_OK;

        auto bMap_it = m_lineResolvedBreakpoints.find(initialBreakpoint.resolved_fullname_index);
        if (bMap_it == m_lineResolvedBreakpoints.end())
            return E_FAIL;

        auto bList_it = bMap_it->second.find(initialBreakpoint.resolved_linenum);
        if (bList_it == bMap_it->second.end())
            return E_FAIL;

        for (auto itList = bList_it->second.begin(); itList != bList_it->second.end();)
        {
            if ((*itList).id == initialBreakpoint.id)
            {
                itList = bList_it->second.erase(itList);
                EnableOneICorBreakpointForLine(bList_it->second);
                break;
            }
            else
                ++itList;
        }

        if (bList_it->second.empty())
            bMap_it->second.erase(bList_it);

        return S_OK;
    };

    HRESULT Status;
    if (lineBreakpoints.empty())
    {
        auto it = m_lineBreakpointMapping.find(filename);
        if (it != m_lineBreakpointMapping.end())
        {
            for (auto &initialBreakpoint : it->second)
            {
                IfFailRet(RemoveResolvedByInitialBreakpoint(initialBreakpoint));
            }
            m_lineBreakpointMapping.erase(it);
        }
        return S_OK;
    }

    auto &breakpointsInSource = m_lineBreakpointMapping[filename];
    std::unordered_map<int, ManagedLineBreakpointMapping*> breakpointsInSourceMap;

    // Remove old breakpoints
    std::unordered_set<int> funcBreakpointLines;
    for (const auto &sb : lineBreakpoints)
    {
        funcBreakpointLines.insert(sb.line);
    }
    for (auto it = breakpointsInSource.begin(); it != breakpointsInSource.end();)
    {
        ManagedLineBreakpointMapping &initialBreakpoint = *it;
        if (funcBreakpointLines.find(initialBreakpoint.breakpoint.line) == funcBreakpointLines.end())
        {
            IfFailRet(RemoveResolvedByInitialBreakpoint(initialBreakpoint));
            it = breakpointsInSource.erase(it);
        }
        else
        {
            breakpointsInSourceMap[initialBreakpoint.breakpoint.line] = &initialBreakpoint;
            ++it;
        }
    }

    // Export breakpoints
    // Note, VSCode and MI/GDB protocols requires, that "breakpoints" and "lineBreakpoints" must have same indexes for same breakpoints.

    for (const auto &sb : lineBreakpoints)
    {
        int line = sb.line;
        Breakpoint breakpoint;

        auto b = breakpointsInSourceMap.find(line);
        if (b == breakpointsInSourceMap.end())
        {
            ManagedLineBreakpointMapping initialBreakpoint;
            initialBreakpoint.breakpoint = sb;
            initialBreakpoint.id = getId();

            // New breakpoint
            ManagedLineBreakpoint bp;
            bp.id = initialBreakpoint.id;
            bp.module = initialBreakpoint.breakpoint.module;
            bp.linenum = line;
            bp.endLine = line;
            bp.condition = initialBreakpoint.breakpoint.condition;
            unsigned resolved_fullname_index = 0;

            if (pProcess && SUCCEEDED(ResolveLineBreakpoint(nullptr, bp, filename, resolved_fullname_index)))
            {
                initialBreakpoint.resolved_fullname_index = resolved_fullname_index;
                initialBreakpoint.resolved_linenum = bp.linenum;
                std::string resolved_fullname;
                m_sharedModules->GetSourceFullPathByIndex(resolved_fullname_index, resolved_fullname);
                bp.ToBreakpoint(breakpoint, resolved_fullname);
                m_lineResolvedBreakpoints[resolved_fullname_index][initialBreakpoint.resolved_linenum].push_back(std::move(bp));
                EnableOneICorBreakpointForLine(m_lineResolvedBreakpoints[resolved_fullname_index][initialBreakpoint.resolved_linenum]);
            }
            else
            {
                bp.ToBreakpoint(breakpoint, filename);
                if (!pProcess)
                    breakpoint.message = "The breakpoint is pending and will be resolved when debugging starts.";
                else
                    breakpoint.message = "The breakpoint will not currently be hit. No symbols have been loaded for this document.";
            }

            breakpointsInSource.insert(breakpointsInSource.begin(), std::move(initialBreakpoint));
        }
        else
        {
            ManagedLineBreakpointMapping &initialBreakpoint = *b->second;
            initialBreakpoint.breakpoint.condition = sb.condition;

            if (initialBreakpoint.resolved_linenum)
            {
                auto bMap_it = m_lineResolvedBreakpoints.find(initialBreakpoint.resolved_fullname_index);
                if (bMap_it == m_lineResolvedBreakpoints.end())
                    return E_FAIL;

                auto bList_it = bMap_it->second.find(initialBreakpoint.resolved_linenum);
                if (bList_it == bMap_it->second.end())
                    return E_FAIL;

                for (auto &bp : bList_it->second)
                {
                    if (initialBreakpoint.id != bp.id)
                        continue;

                    // Existing breakpoint
                    bp.condition = initialBreakpoint.breakpoint.condition;
                    std::string resolved_fullname;
                    m_sharedModules->GetSourceFullPathByIndex(initialBreakpoint.resolved_fullname_index, resolved_fullname);
                    bp.ToBreakpoint(breakpoint, resolved_fullname);
                    break;
                }
            }
            else
            {
                // Was already added, but was not yet resolved.
                ManagedLineBreakpoint bp;
                bp.id = initialBreakpoint.id;
                bp.module = initialBreakpoint.breakpoint.module;
                bp.linenum = line;
                bp.endLine = line;
                bp.condition = initialBreakpoint.breakpoint.condition;
                bp.ToBreakpoint(breakpoint, filename);
                if (!pProcess)
                    breakpoint.message = "The breakpoint is pending and will be resolved when debugging starts.";
                else
                    breakpoint.message = "The breakpoint will not currently be hit. No symbols have been loaded for this document.";
            }
        }

        breakpoints.push_back(breakpoint);
    }

    return S_OK;
}

HRESULT LineBreakpoints::AllBreakpointsActivate(bool act)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    HRESULT Status = S_OK;
    // resolved breakpoints
    for (auto &file_bps : m_lineResolvedBreakpoints)
    {
        for(auto &line_bps : file_bps.second)
        {
            for (auto &rbp : line_bps.second)
            {
                rbp.enabled = act;
            }
            HRESULT ret = EnableOneICorBreakpointForLine(line_bps.second);
            Status = FAILED(ret) ? ret : Status;
        }
    }

    // mapping (for both - resolved and unresolved breakpoints)
    for (auto &file_bps : m_lineBreakpointMapping)
    {
        for (auto &bp: file_bps.second)
        {
            bp.enabled = act;
        }
    }

    return Status;
}

HRESULT LineBreakpoints::BreakpointActivate(uint32_t id, bool act)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto activateResolved = [&](ManagedLineBreakpointMapping &bp) -> HRESULT
    {
        auto bMap_it = m_lineResolvedBreakpoints.find(bp.resolved_fullname_index);
        if (bMap_it == m_lineResolvedBreakpoints.end())
            return E_FAIL;

        auto bList_it = bMap_it->second.find(bp.resolved_linenum);
        if (bList_it == bMap_it->second.end())
            return E_FAIL;

        for(auto &rbp : bList_it->second)
        {
            if (rbp.id != bp.id)
                continue;

            rbp.enabled = act;
            return EnableOneICorBreakpointForLine(bList_it->second);
        }

        return E_FAIL;
    };

    auto activateAllMapped = [&]() -> HRESULT
    {
        for (auto &file_bps : m_lineBreakpointMapping)
        {
            for (auto &bp: file_bps.second)
            {
                if (bp.id != id)
                    continue;

                bp.enabled = act;
                if (!bp.resolved_linenum)
                    return S_OK; // no resolved breakpoint, we done with success
                else
                    return activateResolved(bp); // use mapped data for fast find resolved breakpoint
            }
        }

        return E_FAIL;
    };

    return activateAllMapped();
}

void LineBreakpoints::AddAllBreakpointsInfo(std::vector<IDebugger::BreakpointInfo> &list)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    // m_lineResolvedBreakpoints should be first
    for (auto &file_bps : m_lineResolvedBreakpoints)
    {
        list.reserve(list.size() + file_bps.second.size());
        std::string resolved_fullname;
        m_sharedModules->GetSourceFullPathByIndex(file_bps.first, resolved_fullname);

        for(auto &line_bps : file_bps.second)
        {
            for(auto &bp : line_bps.second)
            {
                list.emplace_back(IDebugger::BreakpointInfo{ bp.id, bp.IsVerified(), bp.enabled, bp.times, bp.condition,
                                                             resolved_fullname, bp.linenum, bp.endLine, bp.module, {} });
            }
        }
    }

    for (auto &file_bps : m_lineBreakpointMapping)
    {
        list.reserve(list.size() + file_bps.second.size());

        for(auto &bp : file_bps.second)
        {
            list.emplace_back(IDebugger::BreakpointInfo{ bp.id, false, true, 0, bp.breakpoint.condition,
                                                         file_bps.first, bp.breakpoint.line, 0, bp.breakpoint.module, {} });
        }
    }

    // remove duplicates (ones from m_lineBreakpointMapping which have resolved pair in m_lineResolvedBreakpoints)
    list.erase(std::unique(list.begin(), list.end()), list.end());
}

} // namespace netcoredbg
