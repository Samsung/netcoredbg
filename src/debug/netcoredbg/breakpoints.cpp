// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "manageddebugger.h"

#include <mutex>
#include <unordered_set>
#include <fstream>


Breakpoints::ManagedBreakpoint::ManagedBreakpoint() :
    id(0), modAddress(0), methodToken(0), ilOffset(0), linenum(0), breakpoint(nullptr), enabled(true), times(0)
{}

Breakpoints::ManagedBreakpoint::~ManagedBreakpoint()
{
    if (breakpoint)
        breakpoint->Activate(0);
}

void Breakpoints::ManagedBreakpoint::ToBreakpoint(Breakpoint &breakpoint)
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsResolved();
    breakpoint.condition = this->condition;
    breakpoint.source = Source(this->fullname);
    breakpoint.line = this->linenum;
    breakpoint.hitCount = this->times;
}

HRESULT Breakpoints::HitBreakpoint(
    Debugger *debugger,
    ICorDebugThread *pThread,
    ICorDebugBreakpoint *pBreakpoint,
    Breakpoint &breakpoint,
    bool &atEntry)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    HRESULT Status;

    atEntry = HitEntry(pThread, pBreakpoint);
    if (atEntry)
        return S_OK;

    ULONG32 ilOffset;
    Modules::SequencePoint sp;
    mdMethodDef methodToken;

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    IfFailRet(m_modules.GetFrameILAndSequencePoint(pFrame, ilOffset, sp));

    auto breakpoints = m_breakpoints.find(sp.document);
    if (breakpoints == m_breakpoints.end())
    {
        // try to find a match with file name only
        breakpoints = m_breakpoints.find(GetFileName(sp.document));
        if (breakpoints == m_breakpoints.end())
            return E_FAIL;
    }

    auto &breakpointsInSource = breakpoints->second;
    auto it = breakpointsInSource.find(sp.startLine);
    if (it == breakpointsInSource.end())
        return E_FAIL;

    ManagedBreakpoint &b = it->second;

    if (b.ilOffset == ilOffset &&
        b.methodToken == methodToken &&
        b.enabled)
    {
        if (!b.condition.empty())
        {
            DWORD threadId = 0;
            IfFailRet(pThread->GetID(&threadId));
            uint64_t frameId = StackFrame(threadId, 0, "").id;

            Variable variable;
            std::string output;
            IfFailRet(debugger->Evaluate(frameId, b.condition, variable, output));

            if (variable.type != "bool" || variable.value != "true")
                return E_FAIL;
        }
        ++b.times;
        b.ToBreakpoint(breakpoint);
        return S_OK;
    }

    return E_FAIL;
}

static HRESULT IsSameFunctionBreakpoint(
    ICorDebugFunctionBreakpoint *pBreakpoint1,
    ICorDebugFunctionBreakpoint *pBreakpoint2)
{
    HRESULT Status;

    if (!pBreakpoint1 || !pBreakpoint2)
        return E_FAIL;

    ULONG32 nOffset1;
    ULONG32 nOffset2;
    IfFailRet(pBreakpoint1->GetOffset(&nOffset1));
    IfFailRet(pBreakpoint2->GetOffset(&nOffset2));

    if (nOffset1 != nOffset2)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunction1;
    ToRelease<ICorDebugFunction> pFunction2;
    IfFailRet(pBreakpoint1->GetFunction(&pFunction1));
    IfFailRet(pBreakpoint2->GetFunction(&pFunction2));

    mdMethodDef methodDef1;
    mdMethodDef methodDef2;
    IfFailRet(pFunction1->GetToken(&methodDef1));
    IfFailRet(pFunction2->GetToken(&methodDef2));

    if (methodDef1 != methodDef2)
        return E_FAIL;

    ToRelease<ICorDebugModule> pModule1;
    ToRelease<ICorDebugModule> pModule2;
    IfFailRet(pFunction1->GetModule(&pModule1));
    IfFailRet(pFunction2->GetModule(&pModule2));

    if (Modules::GetModuleFileName(pModule1) != Modules::GetModuleFileName(pModule2))
        return E_FAIL;

    return S_OK;
}

bool Breakpoints::HitEntry(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    if (!m_stopAtEntry)
        return false;

    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    if (FAILED(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID*) &pFunctionBreakpoint)))
        return false;

    if (FAILED(IsSameFunctionBreakpoint(pFunctionBreakpoint, m_entryBreakpoint)))
        return false;

    m_entryBreakpoint->Activate(0);
    m_entryBreakpoint.Release();
    return true;
}

void ManagedDebugger::InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint)
{
    m_breakpoints.InsertExceptionBreakpoint(name, breakpoint);
}

void Breakpoints::InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    breakpoint.id = m_nextBreakpointId++;
}

void Breakpoints::DeleteAllBreakpoints()
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    m_breakpoints.clear();

    if (m_entryBreakpoint)
        m_entryBreakpoint.Release();
    m_entryPoint = mdMethodDefNil;
}

HRESULT Breakpoints::ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    IfFailRet(m_modules.GetLocationInModule(
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

void Breakpoints::SetStopAtEntry(bool stopAtEntry)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    m_stopAtEntry = stopAtEntry;
}

static mdMethodDef GetEntryPointTokenFromFile(const std::string &path)
{
    std::ifstream f(path, std::ifstream::binary);

    if (!f)
        return mdMethodDefNil;

    IMAGE_DOS_HEADER dosHeader;
    IMAGE_NT_HEADERS32 ntHeaders;

    if (!f.read((char*)&dosHeader, sizeof(dosHeader))) return mdMethodDefNil;
    if (!f.seekg(VAL32(dosHeader.e_lfanew), f.beg)) return mdMethodDefNil;
    if (!f.read((char*)&ntHeaders, sizeof(ntHeaders))) return mdMethodDefNil;

    ULONG corRVA = 0;
    if (ntHeaders.OptionalHeader.Magic == VAL16(IMAGE_NT_OPTIONAL_HDR32_MAGIC))
    {
        corRVA = VAL32(ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }
    else
    {
        IMAGE_NT_HEADERS64 ntHeaders64;
        if (!f.seekg(VAL32(dosHeader.e_lfanew), f.beg)) return mdMethodDefNil;
        if (!f.read((char*)&ntHeaders64, sizeof(ntHeaders64))) return mdMethodDefNil;
        corRVA = VAL32(ntHeaders64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }

    ULONG pos =
        VAL32(dosHeader.e_lfanew)
        + sizeof(ntHeaders.Signature)
        + sizeof(ntHeaders.FileHeader)
        + VAL16(ntHeaders.FileHeader.SizeOfOptionalHeader);

    if (!f.seekg(pos, f.beg)) return mdMethodDefNil;

    for (int i = 0; i < VAL16(ntHeaders.FileHeader.NumberOfSections); i++)
    {
        IMAGE_SECTION_HEADER sectionHeader;

        if (!f.read((char*)&sectionHeader, sizeof(sectionHeader))) return mdMethodDefNil;

        if (corRVA >= VAL32(sectionHeader.VirtualAddress) &&
            corRVA < VAL32(sectionHeader.VirtualAddress) + VAL32(sectionHeader.SizeOfRawData))
        {
            ULONG offset = (corRVA - VAL32(sectionHeader.VirtualAddress)) + VAL32(sectionHeader.PointerToRawData);

            IMAGE_COR20_HEADER corHeader;
            if (!f.seekg(offset, f.beg)) return mdMethodDefNil;
            if (!f.read((char*)&corHeader, sizeof(corHeader))) return mdMethodDefNil;

            if (VAL32(corHeader.Flags) & COMIMAGE_FLAGS_NATIVE_ENTRYPOINT)
                return mdMethodDefNil;

            return VAL32(corHeader.EntryPointToken);
        }
    }

    return mdMethodDefNil;
}

HRESULT Breakpoints::TrySetupEntryBreakpoint(ICorDebugModule *pModule)
{
    if (!m_stopAtEntry || m_entryPoint != mdMethodDefNil)
        return S_FALSE;

    HRESULT Status;

    mdMethodDef entryPointToken = GetEntryPointTokenFromFile(Modules::GetModuleFileName(pModule));
    if (entryPointToken == mdMethodDefNil)
        return S_FALSE;

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pModule->GetFunctionFromToken(entryPointToken, &pFunction));

    ToRelease<ICorDebugFunctionBreakpoint> entryBreakpoint;
    IfFailRet(pFunction->CreateBreakpoint(&entryBreakpoint));

    m_entryPoint = entryPointToken;
    m_entryBreakpoint = entryBreakpoint.Detach();

    return S_OK;
}

void Breakpoints::TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events)
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
                events.emplace_back(BreakpointChanged, breakpoint);
            }
        }
    }

    TrySetupEntryBreakpoint(pModule);
}

HRESULT Breakpoints::ResolveBreakpoint(ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    ToRelease<ICorDebugModule> pModule;

    IfFailRet(m_modules.GetLocationInAny(
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

HRESULT ManagedDebugger::SetBreakpoints(
    std::string filename,
    const std::vector<SourceBreakpoint> &srcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    return m_breakpoints.SetBreakpoints(m_pProcess, filename, srcBreakpoints, breakpoints);
}

HRESULT Breakpoints::SetBreakpoints(
    ICorDebugProcess *pProcess,
    std::string filename,
    const std::vector<SourceBreakpoint> &srcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    if (srcBreakpoints.empty())
    {
        auto it = m_breakpoints.find(filename);
        if (it != m_breakpoints.end())
            m_breakpoints.erase(it);
        return S_OK;
    }

    auto &breakpointsInSource = m_breakpoints[filename];

    // Remove old breakpoints
    std::unordered_set<int> unchangedLines;
    for (const auto &sb : srcBreakpoints)
    {
        int line = sb.line;
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

    for (const auto &sb : srcBreakpoints)
    {
        int line = sb.line;
        Breakpoint breakpoint;

        auto b = breakpointsInSource.find(line);
        if (b == breakpointsInSource.end())
        {
            // New breakpoint
            ManagedBreakpoint bp;
            bp.id = m_nextBreakpointId++;
            bp.fullname = filename;
            bp.linenum = line;
            bp.condition = sb.condition;

            if (pProcess)
                ResolveBreakpoint(bp);

            bp.ToBreakpoint(breakpoint);
            breakpointsInSource.insert(std::make_pair(line, std::move(bp)));
        }
        else
        {
            // Existing breakpoint
            ManagedBreakpoint &bp = b->second;
            bp.condition = sb.condition;
            bp.ToBreakpoint(breakpoint);
        }

        breakpoints.push_back(breakpoint);
    }

    return S_OK;
}
