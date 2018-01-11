// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>

#include "debugger.h"
#include "modules.h"
#include "breakpoints.h"


static std::mutex g_breakMutex;
static ULONG32 g_breakIndex = 1;

struct ManagedBreakpoint {
    ULONG32 id;
    CORDB_ADDRESS modAddress;
    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;
    int linenum;
    ToRelease<ICorDebugBreakpoint> breakpoint;
    bool enabled;
    ULONG32 times;

    bool IsResolved() const
    {
        return modAddress != 0;
    }

    ManagedBreakpoint() :
        id(0), modAddress(0), methodToken(0), ilOffset(0), linenum(0), breakpoint(nullptr), enabled(true), times(0) {}

    ~ManagedBreakpoint()
    {
        if (breakpoint)
            breakpoint->Activate(0);
    }

    void ToBreakpoint(Breakpoint &breakpoint)
    {
        breakpoint.id = this->id;
        breakpoint.verified = this->IsResolved();
        breakpoint.source = Source(this->fullname);
        breakpoint.line = this->linenum;
        breakpoint.hitCount = this->times;
    }

    ManagedBreakpoint(ManagedBreakpoint &&that) = default;

    ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
};

static std::map<ULONG32, ManagedBreakpoint> g_breaks;

HRESULT GetCurrentBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint)
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

    std::lock_guard<std::mutex> lock(g_breakMutex);

    for (auto &it : g_breaks)
    {
        ManagedBreakpoint &b = it.second;

        if (b.fullname == sp.document &&
            b.ilOffset == ilOffset &&
            b.methodToken == methodToken &&
            b.linenum == sp.startLine &&
            b.enabled)
        {
            b.ToBreakpoint(breakpoint);
            return S_OK;
        }
    }

    return E_FAIL;
}

HRESULT HitBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint)
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

    std::lock_guard<std::mutex> lock(g_breakMutex);

    for (auto &it : g_breaks)
    {
        ManagedBreakpoint &b = it.second;

        if (b.fullname == sp.document &&
            b.ilOffset == ilOffset &&
            b.methodToken == methodToken &&
            b.linenum == sp.startLine &&
            b.enabled)
        {
            ++b.times;
            b.ToBreakpoint(breakpoint);
            return S_OK;
        }
    }

    return E_FAIL;
}

static void InsertBreakpoint(ManagedBreakpoint &bp, Breakpoint &breakpoint)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);
    ULONG32 id = g_breakIndex++;
    bp.id = id;
    g_breaks.insert(std::make_pair(id, std::move(bp)));
    bp.ToBreakpoint(breakpoint);
}

void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint)
{
    ManagedBreakpoint bp;
    InsertBreakpoint(bp, breakpoint);
}

HRESULT DeleteBreakpoint(ULONG32 id)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    g_breaks.erase(id);

    return S_OK;
}

void DeleteAllBreakpoints()
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    g_breaks.clear();
}

static HRESULT ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    IfFailRet(Modules::GetLocationInModule(
        pModule, bp.fullname,
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

static HRESULT ResolveBreakpoint(ManagedBreakpoint &bp)
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

void TryResolveBreakpointsForModule(ICorDebugModule *pModule)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    for (auto &it : g_breaks)
    {
        ManagedBreakpoint &b = it.second;

        if (b.IsResolved())
            continue;

        if (SUCCEEDED(ResolveBreakpointInModule(pModule, b)))
        {
            Breakpoint breakpoint;
            b.ToBreakpoint(breakpoint);
            Debugger::EmitBreakpointEvent(BreakpointEvent(BreakpointChanged, breakpoint));
        }
    }
}

HRESULT InsertBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, Breakpoint &breakpoint)
{
    ManagedBreakpoint bp;
    bp.fullname = filename;
    bp.linenum = linenum;

    if (pProcess)
        ResolveBreakpoint(bp);

    InsertBreakpoint(bp, breakpoint);

    return S_OK;
}
