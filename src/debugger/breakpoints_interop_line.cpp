// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_interop_line.h"
#include "debugger/breakpoints_interop.h"
#include "metadata/interop_libraries.h"
#include "utils/logger.h"
#include <algorithm>


namespace netcoredbg
{
namespace InteropDebugging
{

void InteropLineBreakpoints::InteropLineBreakpoint::ToBreakpoint(Breakpoint &breakpoint, bool verified) const
{
    breakpoint.id = this->m_id;
    breakpoint.verified = verified;
    // TODO `breakpoint.condition = m_condition` support
    breakpoint.source = Source(m_sourceFullPath);
    breakpoint.line = this->m_linenum;
    breakpoint.endLine = this->m_endLine;
    breakpoint.hitCount = this->m_times;
}

// Must be called only in case all threads stopped and fixed (see InteropDebugger::StopAndDetach()).
void InteropLineBreakpoints::RemoveAllAtDetach(pid_t pid)
{
    m_breakpointsMutex.lock();

    if (pid != 0)
    {
        for (const auto &entry : m_lineResolvedBreakpoints)
        {
            for (const auto &bp : entry.second)
            {
                if (bp.m_enabled)
                    m_sharedInteropBreakpoints->Remove(pid, entry.first, [](){}, [](std::uintptr_t){});
            }
        }
    }
    m_lineResolvedBreakpoints.clear();
    m_lineBreakpointMapping.clear();

    m_breakpointsMutex.unlock();
}

int InteropLineBreakpoints::AllBreakpointsActivate(pid_t pid, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    int err_code = 0;
    std::unordered_set<uint32_t> failedIDs;

    assert((pid == 0 && m_lineResolvedBreakpoints.empty()) ||
           (pid != 0 && !m_lineResolvedBreakpoints.empty()));

    // resolved breakpoints
    for (auto &addr_bps : m_lineResolvedBreakpoints)
    {
        for (auto &bp : addr_bps.second)
        {
            int tmp = 0;
            if (bp.m_enabled && !act)
                tmp = m_sharedInteropBreakpoints->Remove(pid, addr_bps.first, StopAllThreads, FixAllThreads);
            else if (!bp.m_enabled && act)
                tmp = m_sharedInteropBreakpoints->Add(pid, addr_bps.first, bp.m_isThumbCode, StopAllThreads);

            if (tmp == 0)
                bp.m_enabled = act;
            else
            {
                err_code = tmp;
                failedIDs.emplace(bp.m_id);
            }
        }
    }

    // mapping (for both - resolved and unresolved breakpoints)
    for (auto &file_bps : m_lineBreakpointMapping)
    {
        for (auto &bp: file_bps.second)
        {
            // Note, in case m_enabled field in resolved breakpoint was not changed by error, don't change it here too.
            if (failedIDs.find(bp.m_id) != failedIDs.end())
                continue;

            bp.m_enabled = act;
        }
    }

    return err_code;
}

int InteropLineBreakpoints::BreakpointActivate(pid_t pid, uint32_t id, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    assert((pid == 0 && m_lineResolvedBreakpoints.empty()) ||
           (pid != 0 && !m_lineResolvedBreakpoints.empty()));

    auto activateResolved = [&](std::uintptr_t resolved_brkAddr) -> int
    {
        auto bList_it = m_lineResolvedBreakpoints.find(resolved_brkAddr);
        if (bList_it == m_lineResolvedBreakpoints.end())
            return ENOENT;

        for (auto &bp : bList_it->second)
        {
            if (bp.m_id != id)
                continue;

            int err_code = 0;
            if (bp.m_enabled && !act)
                err_code = m_sharedInteropBreakpoints->Remove(pid, resolved_brkAddr, StopAllThreads, FixAllThreads);
            else if (!bp.m_enabled && act)
                err_code = m_sharedInteropBreakpoints->Add(pid, resolved_brkAddr, bp.m_isThumbCode, StopAllThreads);

            if (err_code == 0)
                bp.m_enabled = act;

            return err_code;
        }

        return ENOENT;
    };

    auto activateAllMapped = [&]() -> int
    {
        for (auto &file_bps : m_lineBreakpointMapping)
        {
            for (auto &bp: file_bps.second)
            {
                if (bp.m_id != id)
                    continue;

                int err_code = 0;
                if (bp.m_resolved_brkAddr)
                    err_code = activateResolved(bp.m_resolved_brkAddr); // use mapped data for fast find resolved breakpoint
                
                if (err_code == 0)
                    bp.m_enabled = act;

                return err_code;
            }
        }

        return ENOENT;
    };

    return activateAllMapped();
}

void InteropLineBreakpoints::AddAllBreakpointsInfo(std::vector<IDebugger::BreakpointInfo> &list)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    // m_lineResolvedBreakpoints should be first
    for (auto &addr_bps : m_lineResolvedBreakpoints)
    {
        list.reserve(list.size() + addr_bps.second.size());
        for(auto &bp : addr_bps.second)
        {
            list.emplace_back(IDebugger::BreakpointInfo{ bp.m_id, true, bp.m_enabled, bp.m_times, "", // TODO bp.m_condition
                                                         bp.m_sourceFullPath, bp.m_linenum, bp.m_endLine, bp.m_module, {} });
        }
    }

    for (auto &file_bps : m_lineBreakpointMapping)
    {
        list.reserve(list.size() + file_bps.second.size());

        for(auto &bp : file_bps.second)
        {
            list.emplace_back(IDebugger::BreakpointInfo{ bp.m_id, false, true, 0, bp.m_breakpoint.condition,
                                                         file_bps.first, bp.m_breakpoint.line, 0, bp.m_breakpoint.module, {} });
        }
    }
}

bool InteropLineBreakpoints::IsLineBreakpoint(std::uintptr_t addr, Breakpoint &breakpoint)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto find = m_lineResolvedBreakpoints.find(addr);
    if (find == m_lineResolvedBreakpoints.end())
        return false;

    for (auto &br : find->second)
    {
        if (!br.m_enabled)
            continue;

        // TODO condition support

        ++br.m_times;
        br.ToBreakpoint(breakpoint, true);
        return true;
    }

    return false;
}

bool InteropLineBreakpoints::SetLineBreakpoints(pid_t pid, InteropLibraries *pInteropLibraries, const std::string &filename, const std::vector<LineBreakpoint> &lineBreakpoints,
                                                std::vector<Breakpoint> &breakpoints, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads, std::function<uint32_t()> getId)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto RemoveResolvedByInitialBreakpoint = [&](InteropLineBreakpointMapping &initialBreakpoint) -> bool
    {
        if (!initialBreakpoint.m_resolved_brkAddr)
            return true;

        auto bList_it = m_lineResolvedBreakpoints.find(initialBreakpoint.m_resolved_brkAddr);
        if (bList_it == m_lineResolvedBreakpoints.end())
            return false;

        for (auto itList = bList_it->second.begin(); itList != bList_it->second.end();)
        {
            if ((*itList).m_id == initialBreakpoint.m_id)
            {
                if ((*itList).m_enabled)
                    m_sharedInteropBreakpoints->Remove(pid, initialBreakpoint.m_resolved_brkAddr, StopAllThreads, FixAllThreads);

                itList = bList_it->second.erase(itList);
                break;
            }
            else
                ++itList;
        }

        if (bList_it->second.empty())
            m_lineResolvedBreakpoints.erase(initialBreakpoint.m_resolved_brkAddr);

        return true;
    };

    if (lineBreakpoints.empty())
    {
        auto it = m_lineBreakpointMapping.find(filename);
        if (it != m_lineBreakpointMapping.end())
        {
            for (auto &initialBreakpoint : it->second)
            {
                if (!RemoveResolvedByInitialBreakpoint(initialBreakpoint))
                {
                    LOGE("Can't remove breakpoint id=%d", initialBreakpoint.m_id);
                    return false;
                }
            }
            m_lineBreakpointMapping.erase(it);
        }
        return true;
    }

    auto &breakpointsInSource = m_lineBreakpointMapping[filename];
    std::unordered_map<int, InteropLineBreakpointMapping*> breakpointsInSourceMap;

    // Remove old breakpoints
    std::unordered_set<int> funcBreakpointLines;
    for (const auto &sb : lineBreakpoints)
    {
        funcBreakpointLines.insert(sb.line);
    }
    for (auto it = breakpointsInSource.begin(); it != breakpointsInSource.end();)
    {
        InteropLineBreakpointMapping &initialBreakpoint = *it;
        if (funcBreakpointLines.find(initialBreakpoint.m_breakpoint.line) == funcBreakpointLines.end())
        {
            if (!RemoveResolvedByInitialBreakpoint(initialBreakpoint))
            {
                LOGE("Can't remove breakpoint id=%d", initialBreakpoint.m_id);
                return false;
            }
            it = breakpointsInSource.erase(it);
        }
        else
        {
            breakpointsInSourceMap[initialBreakpoint.m_breakpoint.line] = &initialBreakpoint;
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
            InteropLineBreakpointMapping initialBreakpoint;
            initialBreakpoint.m_breakpoint = sb;
            initialBreakpoint.m_id = getId();

            // New breakpoint
            InteropLineBreakpoint bp;
            bp.m_id = initialBreakpoint.m_id;
            bp.m_module = initialBreakpoint.m_breakpoint.module;
            bp.m_linenum = line;
            bp.m_endLine = line;
            // TODO condition

            unsigned resolvedLineNum = 0;
            std::string resolvedFullPath;
            bool resolvedIsThumbCode = false;
            std::uintptr_t resolved_brkAddr = pInteropLibraries->FindAddrBySourceAndLine(filename, bp.m_linenum, resolvedLineNum, resolvedFullPath, resolvedIsThumbCode);

            // TODO add multi-line code support (endLine)

            if (pid && resolved_brkAddr)
            {
                if (bp.m_enabled)
                    m_sharedInteropBreakpoints->Add(pid, resolved_brkAddr, resolvedIsThumbCode, StopAllThreads);

                bp.m_linenum = resolvedLineNum;
                // TODO bp.m_endLine -?
                bp.m_endLine = resolvedLineNum;
                bp.m_sourceFullPath = std::move(resolvedFullPath);
                bp.m_isThumbCode = resolvedIsThumbCode;

                initialBreakpoint.m_resolved_brkAddr = resolved_brkAddr;

                bp.ToBreakpoint(breakpoint, true);
                m_lineResolvedBreakpoints[resolved_brkAddr].push_back(std::move(bp));
            }
            else
            {
                bp.m_sourceFullPath = filename;
                bp.ToBreakpoint(breakpoint, false);
                if (!pid)
                    breakpoint.message = "The breakpoint is pending and will be resolved when debugging starts.";
                else
                    breakpoint.message = "The breakpoint will not currently be hit. No symbols have been loaded for this document.";
            }

            breakpointsInSource.push_back(std::move(initialBreakpoint));
        }
        else
        {
            InteropLineBreakpointMapping &initialBreakpoint = *b->second;
            // TODO condition

            if (initialBreakpoint.m_resolved_brkAddr)
            {
                auto bList_it = m_lineResolvedBreakpoints.find(initialBreakpoint.m_resolved_brkAddr);
                if (bList_it == m_lineResolvedBreakpoints.end())
                    return false;

                for (auto &bp : bList_it->second)
                {
                    if (initialBreakpoint.m_id != bp.m_id)
                        continue;

                    // Existing breakpoint
                    // TODO add condition change support
                    bp.ToBreakpoint(breakpoint, true);
                    break;
                }
            }
            else
            {
                // Was already added, but was not yet resolved.
                InteropLineBreakpoint bp;
                bp.m_id = initialBreakpoint.m_id;
                bp.m_module = initialBreakpoint.m_breakpoint.module;
                bp.m_linenum = line;
                bp.m_endLine = line;
                // TODO condition
                bp.m_sourceFullPath = filename;

                bp.ToBreakpoint(breakpoint, false);
                if (!pid)
                    breakpoint.message = "The breakpoint is pending and will be resolved when debugging starts.";
                else
                    breakpoint.message = "The breakpoint will not currently be hit. No symbols have been loaded for this document.";
            }
        }

        breakpoints.push_back(breakpoint);
    }

    return true;
}

void InteropLineBreakpoints::LoadModule(pid_t pid, std::uintptr_t startAddr, InteropLibraries *pInteropLibraries, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &initialBreakpoints : m_lineBreakpointMapping)
    {
        for (auto &initialBreakpoint : initialBreakpoints.second)
        {
            if (initialBreakpoint.m_resolved_brkAddr)
                continue;

            InteropLineBreakpoint bp;
            bp.m_id = initialBreakpoint.m_id;
            bp.m_module = initialBreakpoint.m_breakpoint.module;
            bp.m_enabled = initialBreakpoint.m_enabled;
            bp.m_linenum = initialBreakpoint.m_breakpoint.line;
            bp.m_endLine = initialBreakpoint.m_breakpoint.line;
            // TODO condition

            unsigned resolvedLineNum = 0;
            std::string resolvedFullPath;
            bool resolvedIsThumbCode = false;
            std::uintptr_t resolved_brkAddr = pInteropLibraries->FindAddrBySourceAndLineForLib(startAddr, initialBreakpoints.first, bp.m_linenum, resolvedLineNum, resolvedFullPath, resolvedIsThumbCode);

            // TODO add multi-line code support (endLine)

            if (!resolved_brkAddr)
                continue;

            if (bp.m_enabled)
            {
                // At this point we add breakpoint in unused memory (we are in the middle of lib load process now), no need to stop other threads.
                m_sharedInteropBreakpoints->Add(pid, resolved_brkAddr, resolvedIsThumbCode, [](){});
            }

            bp.m_linenum = resolvedLineNum;
            // TODO bp.m_endLine -?
            bp.m_endLine = resolvedLineNum;
            bp.m_sourceFullPath = std::move(resolvedFullPath);
            bp.m_isThumbCode = resolvedIsThumbCode;

            initialBreakpoint.m_resolved_brkAddr = resolved_brkAddr;

            Breakpoint breakpoint;
            bp.ToBreakpoint(breakpoint, true);
            events.emplace_back(BreakpointChanged, breakpoint);

            m_lineResolvedBreakpoints[resolved_brkAddr].push_back(std::move(bp));
        }
    }
}

void InteropLineBreakpoints::UnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    std::size_t rBrkCount = m_lineResolvedBreakpoints.size();
    for (auto it = m_lineResolvedBreakpoints.begin(); it != m_lineResolvedBreakpoints.end();)
    {
        if (it->first >= startAddr && it->first < endAddr)
            it = m_lineResolvedBreakpoints.erase(it);
        else
            ++it;
    }
    if (rBrkCount == m_lineResolvedBreakpoints.size())
        return;

    for (auto &file_bps : m_lineBreakpointMapping)
    {
        for (auto &bp : file_bps.second)
        {
            if (bp.m_resolved_brkAddr < startAddr || bp.m_resolved_brkAddr > endAddr)
                continue;

            Breakpoint breakpoint;
            breakpoint.id = bp.m_id;
            breakpoint.verified = false;
            breakpoint.condition = bp.m_breakpoint.condition;
            breakpoint.source = file_bps.first;
            breakpoint.line = bp.m_breakpoint.line;
            breakpoint.endLine = bp.m_breakpoint.line;
            breakpoint.hitCount = 0;
            breakpoint.message = "No executable code of the debugger's target code type is associated with this line.";
            events.emplace_back(BreakpointChanged, breakpoint);
            // reset resolve status
            bp.m_resolved_brkAddr = 0;
        }
    }
}

} // namespace InteropDebugging
} // namespace netcoredbg
