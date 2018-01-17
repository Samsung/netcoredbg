// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <mutex>
#include <memory>
#include <unordered_set>

#include "debugger.h"
#include "modules.h"


Debugger::ManagedBreakpoint::ManagedBreakpoint() :
    id(0), modAddress(0), methodToken(0), ilOffset(0), linenum(0), breakpoint(nullptr), enabled(true), times(0)
{}

Debugger::ManagedBreakpoint::~ManagedBreakpoint()
{
    if (breakpoint)
        breakpoint->Activate(0);
}

void Debugger::ManagedBreakpoint::ToBreakpoint(Breakpoint &breakpoint)
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsResolved();
    breakpoint.source = Source(this->fullname);
    breakpoint.line = this->linenum;
    breakpoint.hitCount = this->times;
}

HRESULT Debugger::HitBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint)
{
    HRESULT Status;

    ULONG32 ilOffset;
    Modules::SequencePoint sp;
    mdMethodDef methodToken;

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    IfFailRet(Modules::GetFrameLocation(pFrame, ilOffset, sp));

    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto breakpoints = m_breakpoints.find(sp.document);
    if (breakpoints == m_breakpoints.end())
        return E_FAIL;

    auto &breakpointsInSource = breakpoints->second;
    auto it = breakpointsInSource.find(sp.startLine);
    if (it == breakpointsInSource.end())
        return E_FAIL;

    ManagedBreakpoint &b = it->second;

    if (b.ilOffset == ilOffset &&
        b.methodToken == methodToken &&
        b.enabled)
    {
        ++b.times;
        b.ToBreakpoint(breakpoint);
        return S_OK;
    }

    return E_FAIL;
}

void Debugger::InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    m_nextBreakpointId++;
}

void Debugger::DeleteAllBreakpoints()
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    m_breakpoints.clear();
}

HRESULT Debugger::ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    IfFailRet(Modules::GetLocationInModule(
        pModule,
        bp.fullname,
        bp.linenum,
        ilOffset,
        methodToken,
        fullname));

    ToRelease<ICorDebugFunction> pFunc;
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));
    IfFailRet(pFunc->GetILCode(&pCode));

    ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(ilOffset, &pBreakpoint));
    IfFailRet(pBreakpoint->Activate(TRUE));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    bp.modAddress = modAddress;
    bp.methodToken = methodToken;
    bp.ilOffset = ilOffset;
    bp.fullname = fullname;
    bp.breakpoint = pBreakpoint.Detach();

    return S_OK;
}

void Debugger::TryResolveBreakpointsForModule(ICorDebugModule *pModule)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &breakpoints : m_breakpoints)
    {
        for (auto &it : breakpoints.second)
        {
            ManagedBreakpoint &b = it.second;

            if (b.IsResolved())
                continue;

            if (SUCCEEDED(ResolveBreakpointInModule(pModule, b)))
            {
                Breakpoint breakpoint;
                b.ToBreakpoint(breakpoint);
                m_protocol->EmitBreakpointEvent(BreakpointEvent(BreakpointChanged, breakpoint));
            }
        }
    }
}

HRESULT Debugger::ResolveBreakpoint(ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    ToRelease<ICorDebugModule> pModule;

    IfFailRet(Modules::GetLocationInAny(
        bp.fullname,
        bp.linenum,
        ilOffset,
        methodToken,
        fullname,
        &pModule));

    ToRelease<ICorDebugFunction> pFunc;
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));
    IfFailRet(pFunc->GetILCode(&pCode));

    ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(ilOffset, &pBreakpoint));
    IfFailRet(pBreakpoint->Activate(TRUE));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    bp.modAddress = modAddress;
    bp.methodToken = methodToken;
    bp.ilOffset = ilOffset;
    bp.fullname = fullname;
    bp.breakpoint = pBreakpoint.Detach();

    return S_OK;
}

HRESULT Debugger::SetBreakpoints(std::string filename,
                                 const std::vector<int> &lines,
                                 std::vector<Breakpoint> &breakpoints)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    if (lines.empty())
    {
        auto it = m_breakpoints.find(filename);
        if (it != m_breakpoints.end())
            m_breakpoints.erase(it);
        return S_OK;
    }

    Source source(filename);

    auto &breakpointsInSource = m_breakpoints[filename];

    // Remove old breakpoints
    std::unordered_set<int> unchangedLines;
    for (int line : lines)
    {
        if (breakpointsInSource.find(line) != breakpointsInSource.end())
            unchangedLines.insert(line);
    }

    std::unordered_set<int> removedLines;
    for (auto &b : breakpointsInSource)
        if (unchangedLines.find(b.first) == unchangedLines.end())
            removedLines.insert(b.first);

    for (int line : removedLines)
        breakpointsInSource.erase(line);

    // Export breakpoints

    for (int line : lines)
    {
        Breakpoint breakpoint;

        auto b = breakpointsInSource.find(line);
        if (b == breakpointsInSource.end())
        {
            // New breakpoint
            ManagedBreakpoint bp;
            bp.id = m_nextBreakpointId++;
            bp.fullname = filename;
            bp.linenum = line;

            if (m_pProcess)
                ResolveBreakpoint(bp);

            bp.ToBreakpoint(breakpoint);
            breakpointsInSource.insert(std::make_pair(line, std::move(bp)));
        }
        else
        {
            // Existing breakpoint
            b->second.ToBreakpoint(breakpoint);
        }

        breakpoints.push_back(breakpoint);
    }

    return S_OK;
}
