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

    ManagedBreakpoint(ManagedBreakpoint &&that) = default;

    ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
};

static std::map<ULONG32, ManagedBreakpoint> g_breaks;

static HRESULT PrintBreakpoint(const ManagedBreakpoint &b, std::string &output)
{
    HRESULT Status;

    std::stringstream ss;

    if (b.IsResolved())
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "func=\"\",fullname=\"" << Debugger::EscapeMIValue(b.fullname) << "\",line=\"" << b.linenum << "\"}";
        Status = S_OK;
    }
    else
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "warning=\"No executable code of the debugger's target code type is associated with this line.\"}";
        Status = S_FALSE;
    }
    output = ss.str();
    return Status;
}

HRESULT PrintBreakpoint(ULONG32 id, std::string &output)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    auto it = g_breaks.find(id);

    if (it == g_breaks.end())
        return E_FAIL;

    return PrintBreakpoint(it->second, output);
}

HRESULT HitBreakpoint(ICorDebugThread *pThread, ULONG32 &id, ULONG32 &times)
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
            id = b.id;
            times = ++b.times;
            return S_OK;
        }
    }

    return E_FAIL;
}

static ULONG32 InsertBreakpoint(ManagedBreakpoint &bp)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);
    ULONG32 id = g_breakIndex++;
    bp.id = id;
    g_breaks.insert(std::make_pair(id, std::move(bp)));
    return id;
}

ULONG32 InsertExceptionBreakpoint(const std::string &name)
{
    ManagedBreakpoint bp;
    return InsertBreakpoint(bp);
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
            std::string output;
            PrintBreakpoint(b, output);
            Debugger::Printf("=breakpoint-modified,%s\n", output.c_str());
        }
    }
}

static HRESULT CreateBreakpointInProcess(ManagedBreakpoint &bp, ULONG32 &id)
{
    if (SUCCEEDED(ResolveBreakpoint(bp)))
    {
        id = InsertBreakpoint(bp);
        return S_OK;
    }
    return S_FALSE;
}

HRESULT InsertBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id)
{
    ManagedBreakpoint bp;
    bp.fullname = filename;
    bp.linenum = linenum;

    HRESULT Status = pProcess ? CreateBreakpointInProcess(bp, id) : S_FALSE;

    if (Status == S_FALSE)
    {
        // Add pending breakpoint
        id = InsertBreakpoint(bp);
    }

    return Status;
}
