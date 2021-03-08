// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints.h"

#include <mutex>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include <iterator>
#include "metadata/typeprinter.h"
#include "utils/logger.h"
#include "utils/utf.h"
#include "managed/interop.h"

#include <palclr.h>

using std::string;

namespace netcoredbg
{

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

Breakpoints::ManagedBreakpoint::ManagedBreakpoint() :
    id(0), modAddress(0), methodToken(0), ilOffset(0), linenum(0), endLine(0), iCorBreakpoint(nullptr), enabled(true), times(0)
{}

Breakpoints::ManagedBreakpoint::~ManagedBreakpoint()
{
    if (iCorBreakpoint)
        iCorBreakpoint->Activate(0);
}

void Breakpoints::ManagedBreakpoint::ToBreakpoint(Breakpoint &breakpoint)
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsResolved();
    breakpoint.condition = this->condition;
    breakpoint.source = Source(this->fullname);
    breakpoint.line = this->linenum;
    breakpoint.endLine = this->endLine;
    breakpoint.hitCount = this->times;
}

void Breakpoints::ManagedFunctionBreakpoint::ToBreakpoint(Breakpoint &breakpoint) const
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsResolved();
    breakpoint.condition = this->condition;
    breakpoint.module = this->module;
    breakpoint.funcname = this->name;
    breakpoint.params = this->params;
}

template <typename BreakpointType>
HRESULT Breakpoints::HandleEnabled(BreakpointType &bp, Debugger *debugger, ICorDebugThread *pThread, Breakpoint &breakpoint)
{
    HRESULT Status;

    if (!bp.condition.empty())
    {
        DWORD threadId = 0;
        IfFailRet(pThread->GetID(&threadId));
        FrameId frameId(ThreadId{threadId}, FrameLevel{0});

        Variable variable;
        std::string output;
        IfFailRet(debugger->Evaluate(frameId, bp.condition, variable, output));

        if (variable.type != "bool" || variable.value != "true")
            return E_FAIL;
    }
    ++bp.times;
    bp.ToBreakpoint(breakpoint);

    return S_OK;
}

HRESULT Breakpoints::HitManagedBreakpoint(Debugger *debugger,
                                          ICorDebugThread *pThread,
                                          ICorDebugFrame *pFrame,
                                          mdMethodDef methodToken,
                                          Breakpoint &breakpoint)
{
    ULONG32 ilOffset;
    Modules::SequencePoint sp;
    HRESULT Status;

    IfFailRet(m_modules.GetFrameILAndSequencePoint(pFrame, ilOffset, sp));

    auto breakpoints = m_srcResolvedBreakpoints.find(sp.document);
    if (breakpoints == m_srcResolvedBreakpoints.end())
        return E_FAIL;

    auto &breakpointsInSource = breakpoints->second;
    auto it = breakpointsInSource.find(sp.startLine);
    if (it == breakpointsInSource.end())
        return E_FAIL;

    std::list<ManagedBreakpoint> &bList = it->second;

    // Same logic as provide vsdbg - only one breakpoint is active for one line, find first active in the list.
    for (auto &b : bList)
    {
        if (b.ilOffset != ilOffset ||
            b.methodToken != methodToken ||
            !b.enabled)
            continue;

        if (SUCCEEDED(HandleEnabled(b, debugger, pThread, breakpoint)))
            return S_OK;
    }

    return E_FAIL;
}

HRESULT Breakpoints::HitManagedFunctionBreakpoint(Debugger *debugger,
                                                  ICorDebugThread *pThread,
                                                  ICorDebugFrame *pFrame,
                                                  ICorDebugBreakpoint *pBreakpoint,
                                                  mdMethodDef methodToken,
                                                  Breakpoint &breakpoint)
{
    HRESULT Status;

    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    if (FAILED(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID *) &pFunctionBreakpoint)))
        return E_FAIL;


    for (auto &fb : m_funcBreakpoints)
    {
        ManagedFunctionBreakpoint &fbp = fb.second;
        std::string params("");

        if (fbp.params != "")
        {
            ToRelease<ICorDebugILFrame> pILFrame;
            IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID *) &pILFrame));

            ULONG cParams = 0;
            ToRelease<ICorDebugValueEnum> pParamEnum;

            IfFailRet(pILFrame->EnumerateArguments(&pParamEnum));
            IfFailRet(pParamEnum->GetCount(&cParams));

            params = "(";

            if (cParams > 0)
            {
                for (ULONG i = 0; i < cParams; ++i)
                {
                    ToRelease<ICorDebugValue> pValue;
                    ULONG cArgsFetched;
                    Status = pParamEnum->Next(1, &pValue, &cArgsFetched);
                    std::string param;

                    if (FAILED(Status))
                        continue;

                    if (Status == S_FALSE)
                        break;

                    IfFailRet(TypePrinter::GetTypeOfValue(pValue, param));
                    if (i > 0)
                        params += ",";

                    params += param;
                }

            }
            params += ")";
        }

        for (auto &fbel : fbp.breakpoints)
        {
            if (SUCCEEDED(IsSameFunctionBreakpoint(pFunctionBreakpoint, fbel.funcBreakpoint)) && fbp.enabled
                && params == fbp.params)
                return HandleEnabled(fbp, debugger, pThread, breakpoint);
        }
    }

    return E_FAIL;
}

HRESULT Breakpoints::HitBreakpoint(Debugger *debugger,
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

    mdMethodDef methodToken;

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    if (SUCCEEDED(HitManagedBreakpoint(debugger, pThread, pFrame, methodToken, breakpoint)))
        return S_OK;

    return HitManagedFunctionBreakpoint(debugger, pThread, pFrame, pBreakpoint, methodToken, breakpoint);
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
    m_entryBreakpoint.Free();
    return true;
}

void Breakpoints::DeleteAllBreakpoints()
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    m_srcResolvedBreakpoints.clear();
    m_srcInitialBreakpoints.clear();

    if (m_entryBreakpoint)
        m_entryBreakpoint.Free();
    m_entryPoint = mdMethodDefNil;
}

HRESULT Breakpoints::ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname = bp.fullname;
    int linenum = bp.linenum;
    int endLine = bp.endLine;

    IfFailRet(m_modules.ResolveBreakpointFileAndLine(fullname, linenum, endLine));

    IfFailRet(m_modules.GetLocationInModule(
        pModule,
        fullname,
        linenum,
        ilOffset,
        methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));
    IfFailRet(pFunc->GetILCode(&pCode));

    ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(ilOffset, &pBreakpoint));
    IfFailRet(pBreakpoint->Activate(TRUE));

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    bp.linenum = linenum;
    bp.endLine = endLine;
    bp.modAddress = modAddress;
    bp.methodToken = methodToken;
    bp.ilOffset = ilOffset;
    bp.fullname = fullname;
    bp.iCorBreakpoint = pBreakpoint.Detach();

    return S_OK;
}

void Breakpoints::SetStopAtEntry(bool stopAtEntry)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    m_stopAtEntry = stopAtEntry;
}

static mdMethodDef GetEntryPointTokenFromFile(const std::string &path)
{
    class scope_guard
    {
    private:
        FILE **ppFile_;

    public:
        scope_guard(FILE **ppFile) : ppFile_(ppFile) {}
        ~scope_guard() {if (*ppFile_) fclose(*ppFile_);}
    };

    FILE *pFile = nullptr;
    scope_guard file(&pFile);

#ifdef WIN32
    if (_wfopen_s(&pFile, to_utf16(path).c_str(), L"rb") != 0)
        return mdMethodDefNil;
#else
    pFile = fopen(path.c_str(), "rb");
#endif // WIN32

    if (!pFile)
        return mdMethodDefNil;

    IMAGE_DOS_HEADER dosHeader;
    IMAGE_NT_HEADERS32 ntHeaders;

    if (fread(&dosHeader, sizeof(dosHeader), 1, pFile) != 1) return mdMethodDefNil;
    if (fseek(pFile, VAL32(dosHeader.e_lfanew), SEEK_SET) != 0) return mdMethodDefNil;
    if (fread(&ntHeaders, sizeof(ntHeaders), 1, pFile) != 1) return mdMethodDefNil;

    ULONG corRVA = 0;
    if (ntHeaders.OptionalHeader.Magic == VAL16(IMAGE_NT_OPTIONAL_HDR32_MAGIC))
    {
        corRVA = VAL32(ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }
    else
    {
        IMAGE_NT_HEADERS64 ntHeaders64;
        if (fseek(pFile, VAL32(dosHeader.e_lfanew), SEEK_SET) != 0) return mdMethodDefNil;
        if (fread(&ntHeaders64, sizeof(ntHeaders64), 1, pFile) != 1) return mdMethodDefNil;
        corRVA = VAL32(ntHeaders64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COMHEADER].VirtualAddress);
    }

    ULONG pos =
        VAL32(dosHeader.e_lfanew)
        + sizeof(ntHeaders.Signature)
        + sizeof(ntHeaders.FileHeader)
        + VAL16(ntHeaders.FileHeader.SizeOfOptionalHeader);

    if (fseek(pFile, pos, SEEK_SET) != 0) return mdMethodDefNil;

    for (int i = 0; i < VAL16(ntHeaders.FileHeader.NumberOfSections); i++)
    {
        IMAGE_SECTION_HEADER sectionHeader;

        if (fread(&sectionHeader, sizeof(sectionHeader), 1, pFile) != 1) return mdMethodDefNil;

        if (corRVA >= VAL32(sectionHeader.VirtualAddress) &&
            corRVA < VAL32(sectionHeader.VirtualAddress) + VAL32(sectionHeader.SizeOfRawData))
        {
            ULONG offset = (corRVA - VAL32(sectionHeader.VirtualAddress)) + VAL32(sectionHeader.PointerToRawData);

            IMAGE_COR20_HEADER corHeader;
            if (fseek(pFile, offset, SEEK_SET) != 0) return mdMethodDefNil;
            if (fread(&corHeader, sizeof(corHeader), 1, pFile) != 1) return mdMethodDefNil;

            if (VAL32(corHeader.Flags) & COMIMAGE_FLAGS_NATIVE_ENTRYPOINT)
                return mdMethodDefNil;

            return VAL32(corHeader.EntryPointToken);
        }
    }

    return mdMethodDefNil;
}

void Breakpoints::EnableOneICorBreakpointForLine(std::list<ManagedBreakpoint> &bList)
{
    // Same logic as provide vsdbg - only one breakpoint is active for one line.
    BOOL needEnable = TRUE;
    for (auto it = bList.begin(); it != bList.end(); ++it)
    {
        if ((*it).iCorBreakpoint)
        {
            (*it).iCorBreakpoint->Activate(needEnable);
            needEnable = FALSE;
        }
    }
}

// Try to setup proper entry breakpoint method token and IL offset for async Main method.
// [in] pModule - module with async Main method;
// [in] pMD - metadata interface for pModule;
// [in] modules - all loaded modules debug related data;
// [in] mdMainClass - class token with Main method in module pModule;
// [out] entryPointToken - corrected method token;
// [out] entryPointOffset - corrected IL offset on first user code line.
static HRESULT TrySetupAsyncEntryBreakpoint(ICorDebugModule *pModule, IMetaDataImport *pMD, Modules &modules, mdTypeDef mdMainClass,
                                            mdMethodDef &entryPointToken, ULONG32 &entryPointOffset)
{
    // In case of async method, compiler use `Namespace.ClassName.<Main>()` as entry method, that call
    // `Namespace.ClassName.Main()`, that create `Namespace.ClassName.<Main>d__0` and start state machine routine.
    // In this case, "real entry method" with user code from initial `Main()` method will be in:
    // Namespace.ClassName.<Main>d__0.MoveNext()
    // Note, number in "<Main>d__0" class name could be different.
    // Note, `Namespace.ClassName` could be different (see `-main` compiler option).
    // Note, `Namespace.ClassName.<Main>d__0` type have enclosing class as method `Namespace.ClassName.<Main>()` class.
    HRESULT Status;
     ULONG numTypedefs = 0;
    HCORENUM hEnum = NULL;
    mdTypeDef typeDef;
    mdMethodDef resultToken = mdMethodDefNil;
    while(SUCCEEDED(pMD->EnumTypeDefs(&hEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0 && resultToken == mdMethodDefNil)
    {
        mdTypeDef mdEnclosingClass;
        if (FAILED(pMD->GetNestedClassProps(typeDef, &mdEnclosingClass) ||
            mdEnclosingClass != mdMainClass))
            continue;

        DWORD flags;
        WCHAR className[mdNameLen];
        ULONG classNameLen;
        IfFailRet(pMD->GetTypeDefProps(typeDef, className, _countof(className), &classNameLen, &flags, NULL));
        if (!starts_with(className, W("<Main>d__")))
            continue;

        ULONG numMethods = 0;
        HCORENUM fEnum = NULL;
        mdMethodDef methodDef;
        while(SUCCEEDED(pMD->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
        {
            mdTypeDef memTypeDef;
            WCHAR funcName[mdNameLen];
            ULONG funcNameLen;
            if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef, funcName, _countof(funcName), &funcNameLen,
                                            nullptr, nullptr, nullptr, nullptr, nullptr)))
            {
                continue;
            }

            if (str_equal(funcName, W("MoveNext")))
            {
                resultToken = methodDef;
                break;
            }
        }
        pMD->CloseEnum(fEnum);
    }
    pMD->CloseEnum(hEnum);

    if (resultToken == mdMethodDefNil)
        return E_FAIL;

    // Note, in case of async `MoveNext` method, user code don't start from 0 IL offset.
    Modules::SequencePoint sequencePoint;
    IfFailRet(modules.GetNextSequencePointInMethod(pModule, resultToken, 0, sequencePoint));

    entryPointToken = resultToken;
    entryPointOffset = sequencePoint.offset;
    return S_OK;
}

HRESULT Breakpoints::TrySetupEntryBreakpoint(ICorDebugModule *pModule)
{
    if (!m_stopAtEntry || m_entryPoint != mdMethodDefNil)
        return S_FALSE;

    HRESULT Status;
    mdMethodDef entryPointToken = GetEntryPointTokenFromFile(Modules::GetModuleFileName(pModule));
    if (entryPointToken == mdMethodDefNil)
        return S_FALSE;

    ULONG32 entryPointOffset = 0;
    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    mdTypeDef mdMainClass;
    WCHAR funcName[mdNameLen];
    ULONG funcNameLen;
    Modules::SequencePoint sequencePoint;
    // If we can't setup entry point correctly for async method, leave it "as is".
    if (SUCCEEDED(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown)) &&
        SUCCEEDED(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD)) &&
        SUCCEEDED(pMD->GetMethodProps(entryPointToken, &mdMainClass, funcName, _countof(funcName), &funcNameLen,
                                      nullptr, nullptr, nullptr, nullptr, nullptr)) &&
        // The `Main` method is the entry point of a C# application. (Libraries and services do not require a Main method as an entry point.)
        // https://docs.microsoft.com/en-us/dotnet/csharp/programming-guide/main-and-command-args/
        // In case of async method as entry method, GetEntryPointTokenFromFile() should return compiler's generated method `<Main>`, plus,
        // this should be method without user code.
        str_equal(funcName, W("<Main>")) &&
        FAILED(m_modules.GetNextSequencePointInMethod(pModule, entryPointToken, 0, sequencePoint)))
    {
        TrySetupAsyncEntryBreakpoint(pModule, pMD, m_modules, mdMainClass, entryPointToken, entryPointOffset);
    }

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pModule->GetFunctionFromToken(entryPointToken, &pFunction));
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunction->GetILCode(&pCode));
    ToRelease<ICorDebugFunctionBreakpoint> entryBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(entryPointOffset, &entryBreakpoint));

    m_entryPoint = entryPointToken;
    m_entryBreakpoint = entryBreakpoint.Detach();

    return S_OK;
}

void Breakpoints::TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &initialBreakpoints : m_srcInitialBreakpoints)
    {
        for (auto &initialBreakpoint : initialBreakpoints.second)
        {
            if (!initialBreakpoint.resolved_fullname.empty())
                continue;

            ManagedBreakpoint bp;
            bp.id = initialBreakpoint.id;
            bp.fullname = initialBreakpoints.first;
            bp.linenum = initialBreakpoint.breakpoint.line;
            bp.endLine = initialBreakpoint.breakpoint.line;
            bp.condition = initialBreakpoint.breakpoint.condition;

            if (SUCCEEDED(ResolveBreakpointInModule(pModule, bp)))
            {
                Breakpoint breakpoint;
                bp.ToBreakpoint(breakpoint);
                events.emplace_back(BreakpointChanged, breakpoint);

                initialBreakpoint.resolved_fullname = bp.fullname;
                initialBreakpoint.resolved_linenum = bp.linenum;

                m_srcResolvedBreakpoints[initialBreakpoint.resolved_fullname][initialBreakpoint.resolved_linenum].push_back(std::move(bp));
                EnableOneICorBreakpointForLine(m_srcResolvedBreakpoints[initialBreakpoint.resolved_fullname][initialBreakpoint.resolved_linenum]);
            }
        }
    }

    for (auto &funcBreakpoints : m_funcBreakpoints)
    {
        ManagedFunctionBreakpoint &fb = funcBreakpoints.second;

        if (fb.IsResolved())
            continue;

        if (SUCCEEDED(ResolveFunctionBreakpointInModule(pModule, fb)))
        {
            Breakpoint breakpoint;
            fb.ToBreakpoint(breakpoint);
            events.emplace_back(BreakpointChanged, breakpoint);
        }
    }

    TrySetupEntryBreakpoint(pModule);
}

HRESULT Breakpoints::ResolveBreakpoint(ManagedBreakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname = bp.fullname;
    int linenum = bp.linenum;
    int endLine = bp.endLine;

    IfFailRet(m_modules.ResolveBreakpointFileAndLine(fullname, linenum, endLine));

    ToRelease<ICorDebugModule> pModule;

    IfFailRet(m_modules.GetLocationInAny(
        fullname,
        linenum,
        ilOffset,
        methodToken,
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

    bp.linenum = linenum;
    bp.endLine = endLine;
    bp.modAddress = modAddress;
    bp.methodToken = methodToken;
    bp.ilOffset = ilOffset;
    bp.fullname = fullname;
    bp.iCorBreakpoint = pBreakpoint.Detach();

    return S_OK;
}

HRESULT Breakpoints::SetBreakpoints(
    ICorDebugProcess *pProcess,
    const std::string& filename,
    const std::vector<SourceBreakpoint> &srcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    auto RemoveResolvedByInitialBreakpoint = [&] (SourceBreakpointMapping &initialBreakpoint)
    {
        if (initialBreakpoint.resolved_fullname.empty())
            return S_OK;

        auto bMap_it = m_srcResolvedBreakpoints.find(initialBreakpoint.resolved_fullname);
        if (bMap_it == m_srcResolvedBreakpoints.end())
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
    if (srcBreakpoints.empty())
    {
        auto it = m_srcInitialBreakpoints.find(filename);
        if (it != m_srcInitialBreakpoints.end())
        {
            for (auto &initialBreakpoint : it->second)
            {
                IfFailRet(RemoveResolvedByInitialBreakpoint(initialBreakpoint));
            }
            m_srcInitialBreakpoints.erase(it);
        }
        return S_OK;
    }

    auto &breakpointsInSource = m_srcInitialBreakpoints[filename];
    std::unordered_map<int, SourceBreakpointMapping*> breakpointsInSourceMap;

    // Remove old breakpoints
    std::unordered_set<int> funcBreakpointLines;
    for (const auto &sb : srcBreakpoints)
    {
        funcBreakpointLines.insert(sb.line);
    }
    for (auto it = breakpointsInSource.begin(); it != breakpointsInSource.end();)
    {
        SourceBreakpointMapping &initialBreakpoint = *it;
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
    // Note, VSCode and MI/GDB protocols requires, that "breakpoints" and "srcBreakpoints" must have same indexes for same breakpoints.

    for (const auto &sb : srcBreakpoints)
    {
        int line = sb.line;
        Breakpoint breakpoint;

        auto b = breakpointsInSourceMap.find(line);
        if (b == breakpointsInSourceMap.end())
        {
            SourceBreakpointMapping initialBreakpoint;
            initialBreakpoint.breakpoint = sb;
            initialBreakpoint.id = m_nextBreakpointId++;

            // New breakpoint
            ManagedBreakpoint bp;
            bp.id = initialBreakpoint.id;
            bp.fullname = filename;
            bp.linenum = line;
            bp.endLine = line;
            bp.condition = initialBreakpoint.breakpoint.condition;

            if (pProcess && SUCCEEDED(ResolveBreakpoint(bp)))
            {
                initialBreakpoint.resolved_fullname = bp.fullname;
                initialBreakpoint.resolved_linenum = bp.linenum;
                bp.ToBreakpoint(breakpoint);
                m_srcResolvedBreakpoints[initialBreakpoint.resolved_fullname][initialBreakpoint.resolved_linenum].push_back(std::move(bp));
                EnableOneICorBreakpointForLine(m_srcResolvedBreakpoints[initialBreakpoint.resolved_fullname][initialBreakpoint.resolved_linenum]);
            }
            else
            {
                bp.ToBreakpoint(breakpoint);
                if (!pProcess)
                    breakpoint.message = "The breakpoint is pending and will be resolved when debugging starts.";
                else
                    breakpoint.message = "The breakpoint will not currently be hit. No symbols have been loaded for this document.";
            }

            breakpointsInSource.insert(breakpointsInSource.begin(), std::move(initialBreakpoint));
        }
        else
        {
            SourceBreakpointMapping &initialBreakpoint = *b->second;
            initialBreakpoint.breakpoint.condition = sb.condition;

            if (!initialBreakpoint.resolved_fullname.empty())
            {
                auto bMap_it = m_srcResolvedBreakpoints.find(initialBreakpoint.resolved_fullname);
                if (bMap_it == m_srcResolvedBreakpoints.end())
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
                    bp.ToBreakpoint(breakpoint);
                    break;
                }
            }
            else
            {
                // Was already added, but was not yet resolved.
                ManagedBreakpoint bp;
                bp.id = initialBreakpoint.id;
                bp.fullname = filename;
                bp.linenum = line;
                bp.endLine = line;
                bp.condition = initialBreakpoint.breakpoint.condition;
                bp.ToBreakpoint(breakpoint);
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


// This is lightweight (it occupies only space for three pointers) class which
// accepts in constructor a reference to one of the classes which store information
// about breakpoints, erases the type (but stores type information within function
// pointer) and allows later to get breakpoint information via `get` function,
// which retursn universal structure describing the breakpoint: BreakpointInfo.
// This structure is generated on demand.
//
// This class is indentent for getting sorted list of the breakpoints.
//
// On input this class accepts the reference to one of the following classes:
// ManagedBreakpoint, SourceBreakpointMapping, ManagedFunctionBreakpoint.
// Such classes is stored in one of the following member variables of Breakpoints class:
// m_srcResolvedBreakpoints, m_srcInitialBreakpoints.size() or m_funcBreakpoints.size().
//
class Breakpoints::AnyBPReference
{
    using BreakpointInfo = Debugger::BreakpointInfo;
    using Key = const std::string *;

    const void *ptr;  // pointer to class which store breakpoint info
    Key key;          // additional key which is required for SourceBreakpointMapping
    BreakpointInfo (*getter)(Key, const void *); // getter function (stores the type for `ptr`)

public:
    AnyBPReference(const ManagedBreakpoint& val) : ptr(&val), key(nullptr)
    {
        getter = [](Key, const void *ptr) -> BreakpointInfo {
            const auto& bp = *reinterpret_cast<const ManagedBreakpoint*>(ptr);
            return { bp.id, true, bp.enabled, bp.times, {}, bp.fullname, bp.linenum, bp.endLine, {}, {} };
        };
    }

    // case for SourceBreakpointMapping: additionaly key is passed
    AnyBPReference(std::tuple<const std::string&, const SourceBreakpointMapping&> val)
    : ptr(&std::get<1>(val)), key(&std::get<0>(val))
    {
        getter = [](Key key, const void *ptr) -> BreakpointInfo {
            const auto& bp = *reinterpret_cast<const SourceBreakpointMapping*>(ptr);
            return { bp.id, false, true, 0, {}, string_view{*key}, bp.breakpoint.line, 0, {}, {} };
        };
    }

    // case for using with std::copy (accepting std::map and similar containers)
    AnyBPReference(const std::pair<const std::string, ManagedFunctionBreakpoint>& pair)
    : ptr(&pair.second), key(nullptr)
    {
        getter = [](Key, const void *ptr) -> BreakpointInfo {
            const auto& bp = *reinterpret_cast<const ManagedFunctionBreakpoint*>(ptr);
            return { bp.id, !bp.breakpoints.empty(), bp.enabled, bp.times, bp.condition, 
                        bp.name, 0, 0, bp.module, bp.params };
        };
    }

    // This function restores the type via call to stored getter function and generates
    // the result from each particular input class pointer to which is stored within this class.
    BreakpointInfo get() const { return getter(key, ptr); }

    // Comparator functions which allow to sort container of `AnyBPReference`.
    bool operator<(const AnyBPReference& other) const { return get().id < other.get().id; }
    bool operator==(const AnyBPReference& other) const { return get().id == other.get().id; }
};


void Breakpoints::EnumerateBreakpoints(std::function<bool (const Debugger::BreakpointInfo&)>&& callback)
{
    // create (empty) list of references to all three types of input data, reserve memory
    std::vector<AnyBPReference> list;
    list.reserve(m_srcResolvedBreakpoints.size()
                  + m_srcInitialBreakpoints.size() + m_funcBreakpoints.size());

    // put contents of m_srcResolvedBreakpoints (should be first), m_srcInitialBreakpoints
    // and m_funcBreakpoints in common unsorted list `list`:
    for (const auto& outer : m_srcResolvedBreakpoints)
    {
        for (const auto& inner : outer.second)
            std::copy(inner.second.begin(), inner.second.end(), std::back_inserter(list));
    }

    for (const auto& outer : m_srcInitialBreakpoints)
    {
        for (const auto& inner : outer.second)
            list.emplace_back(std::forward_as_tuple(outer.first, inner));
    }

    std::copy(m_funcBreakpoints.begin(), m_funcBreakpoints.end(), std::back_inserter(list));

    // sort breakpoint list by ascending order, preserve order of elements with same number
    std::stable_sort(list.begin(), list.end());

    // remove duplicates (ones from m_srcInitialBreakpoints which have
    // resolved pair in m_srcResolvedBreakpoints)
    list.erase(std::unique(list.begin(), list.end()), list.end());

    // apply callback function for each breakpoint
    for (const auto& item : list)
    {
        if (!callback(item.get()))
            break;
    }
}


HRESULT Breakpoints::ResolveFunctionBreakpoint(ManagedFunctionBreakpoint &fbp)
{
    HRESULT Status;

    IfFailRet(m_modules.ResolveFunctionInAny(fbp.module, fbp.name, [&](
        ICorDebugModule *pModule,
        mdMethodDef &methodToken) -> HRESULT
    {
        ToRelease<ICorDebugFunction> pFunc;
        IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));

        ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
        IfFailRet(pFunc->CreateBreakpoint(&pFunctionBreakpoint));
        IfFailRet(pFunctionBreakpoint->Activate(TRUE));

        CORDB_ADDRESS modAddress;
        IfFailRet(pModule->GetBaseAddress(&modAddress));

        fbp.breakpoints.emplace_back(modAddress, methodToken, pFunctionBreakpoint.Detach());

        return S_OK;
    }));


    return S_OK;
}

HRESULT Breakpoints::ResolveFunctionBreakpointInModule(ICorDebugModule *pModule, ManagedFunctionBreakpoint &fbp)
{
    HRESULT Status;

    IfFailRet(m_modules.ResolveFunctionInModule(
        pModule,
        fbp.module,
        fbp.name,
        [&] (ICorDebugModule *pModule, mdMethodDef &methodToken) -> HRESULT
    {

        ToRelease<ICorDebugFunction> pFunc;
        IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));

        ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
        IfFailRet(pFunc->CreateBreakpoint(&pFunctionBreakpoint));
        IfFailRet(pFunctionBreakpoint->Activate(TRUE));

        CORDB_ADDRESS modAddress;
        IfFailRet(pModule->GetBaseAddress(&modAddress));

        fbp.breakpoints.emplace_back(modAddress, methodToken, pFunctionBreakpoint.Detach());

        return S_OK;
    }));

    return S_OK;
}

HRESULT Breakpoints::SetFunctionBreakpoints(
    ICorDebugProcess *pProcess,
    const std::vector<FunctionBreakpoint> &funcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    // Remove old breakpoints
    std::unordered_set<std::string> funcBreakpointFuncs;
    for (const auto &fb : funcBreakpoints)
    {
        std::string fullFuncName("");
        if (fb.module != "")
        {
            fullFuncName = fb.module + "!";
        }
        fullFuncName += fb.func + fb.params;
        funcBreakpointFuncs.insert(fullFuncName);
    }
    for (auto it = m_funcBreakpoints.begin(); it != m_funcBreakpoints.end();)
    {
        if (funcBreakpointFuncs.find(it->first) == funcBreakpointFuncs.end())
            it = m_funcBreakpoints.erase(it);
        else
            ++it;
    }

    if (funcBreakpoints.empty())
        return S_OK;


    // Export function breakpoints
    // Note, VSCode and MI/GDB protocols requires, that "breakpoints" and "funcBreakpoints" must have same indexes for same breakpoints.

    for (const auto &fb : funcBreakpoints)
    {
        std::string fullFuncName("");

        if (fb.module != "")
            fullFuncName = fb.module + "!";

        fullFuncName += fb.func + fb.params;

        Breakpoint breakpoint;

        auto b = m_funcBreakpoints.find(fullFuncName);
        if (b == m_funcBreakpoints.end())
        {
            // New function breakpoint
            ManagedFunctionBreakpoint fbp;

            fbp.id = m_nextBreakpointId++;
            fbp.module = fb.module;
            fbp.name = fb.func;
            fbp.params = fb.params;
            fbp.condition = fb.condition;

            if (pProcess)
                ResolveFunctionBreakpoint(fbp);

            fbp.ToBreakpoint(breakpoint);
            m_funcBreakpoints.insert(std::make_pair(fullFuncName, std::move(fbp)));
        }
        else
        {
            ManagedFunctionBreakpoint &fbp = b->second;

            fbp.condition = fb.condition;
            fbp.ToBreakpoint(breakpoint);
        }

        breakpoints.push_back(breakpoint);
    }


    return S_OK;
}

HRESULT Breakpoints::InsertExceptionBreakpoint(const ExceptionBreakMode &mode,
    const string &name, uint32_t &rid)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    HRESULT Status;
    IfFailRet(m_exceptionBreakpoints.Insert(m_nextBreakpointId, mode, name));
    rid = m_nextBreakpointId;
    ++m_nextBreakpointId;
    return S_OK;
}

HRESULT Breakpoints::DeleteExceptionBreakpoint(const uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    return m_exceptionBreakpoints.Delete(id);
}

HRESULT Breakpoints::GetExceptionBreakMode(ExceptionBreakMode &mode,
    const string &name)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    return m_exceptionBreakpoints.GetExceptionBreakMode(mode, name);
}

bool Breakpoints::MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const string &name,
    const ExceptionBreakCategory category)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);
    return m_exceptionBreakpoints.Match(dwEventType, name, category);
}

} // namespace netcoredbg
