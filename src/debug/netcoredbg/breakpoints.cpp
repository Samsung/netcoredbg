#include "common.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <map>


// Modules
HRESULT GetFrameLocation(ICorDebugFrame *pFrame,
                         ULONG32 &ilOffset,
                         mdMethodDef &methodToken,
                         std::string &fullname,
                         ULONG &linenum);
std::string GetModuleName(ICorDebugModule *pModule);

HRESULT GetLocationInModule(ICorDebugModule *pModule,
                            std::string filename,
                            ULONG linenum,
                            ULONG32 &ilOffset,
                            mdMethodDef &methodToken,
                            std::string &fullname);


static std::mutex g_breakMutex;
static ULONG32 g_breakIndex = 1;

struct Breakpoint {
    ULONG32 id;
    CORDB_ADDRESS modAddress;
    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;
    int linenum;
    ToRelease<ICorDebugBreakpoint> breakpoint;

    bool IsResolved() const
    {
        return modAddress != 0;
    }

    Breakpoint() : id(0), modAddress(0), methodToken(0), ilOffset(0), linenum(0), breakpoint(nullptr) {}

    ~Breakpoint()
    {
        if (breakpoint)
            breakpoint->Activate(0);
    }

    Breakpoint(Breakpoint &&that) = default;

    Breakpoint(const Breakpoint &that) = delete;
};

static std::map<ULONG32, Breakpoint> g_breaks;

HRESULT PrintBreakpoint(ULONG32 id, std::string &output)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    auto it = g_breaks.find(id);

    if (it == g_breaks.end())
        return E_FAIL;

    Breakpoint &b = it->second;

    std::stringstream ss;

    HRESULT Status;
    if (b.IsResolved())
    {
        ss << "bkpt={number=\"" << id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "func=\"\",fullname=\"" << b.fullname << "\",line=\"" << b.linenum << "\"}";
        Status = S_OK;
    }
    else
    {
        ss << "bkpt={number=\"" << id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "warning=\"No executable code of the debugger's target code type is associated with this line.\"}";
        Status = S_FALSE;
    }
    output = ss.str();
    return S_OK;
}

HRESULT FindCurrentBreakpointId(ICorDebugThread *pThread, ULONG32 &id)
{
    HRESULT Status;
    ULONG32 ilOffset;
    mdMethodDef methodToken;
    std::string fullname;
    ULONG linenum;

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    IfFailRet(GetFrameLocation(pFrame, ilOffset, methodToken, fullname, linenum));

    std::lock_guard<std::mutex> lock(g_breakMutex);

    for (auto &it : g_breaks)
    {
        Breakpoint &b = it.second;

        if (b.fullname == fullname &&
            b.ilOffset == ilOffset &&
            b.methodToken == methodToken &&
            b.linenum == linenum)
        {
            id = b.id;
            return S_OK;
        }
    }

    return E_FAIL;
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

HRESULT ResolveBreakpoint(ICorDebugModule *pModule, std::string filename, int linenum, Breakpoint &bp)
{
    HRESULT Status;

    mdMethodDef methodToken;
    ULONG32 ilOffset;
    std::string fullname;

    IfFailRet(GetLocationInModule(pModule, filename,
                                  linenum,
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
    bp.linenum = linenum;
    bp.breakpoint = pBreakpoint.Detach();

    return S_OK;
}

HRESULT TryResolveBreakpointsForModule(ICorDebugModule *pModule)
{
    std::lock_guard<std::mutex> lock(g_breakMutex);

    for (auto &it : g_breaks)
    {
        Breakpoint &b = it.second;

        if (b.IsResolved())
            continue;

        if (SUCCEEDED(ResolveBreakpoint(pModule, b.fullname, b.linenum, b)))
        {
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    Breakpoint bp;
    bp.fullname = filename;
    bp.linenum = linenum;

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);

        ToRelease<ICorDebugAssemblyEnum> assemblies;
        IfFailRet(pDomain->EnumerateAssemblies(&assemblies));

        ICorDebugAssembly *curAssembly;
        ULONG assembliesFetched;
        while (SUCCEEDED(assemblies->Next(1, &curAssembly, &assembliesFetched)) && assembliesFetched == 1)
        {
            ToRelease<ICorDebugAssembly> pAssembly(curAssembly);

            ToRelease<ICorDebugModuleEnum> modules;
            IfFailRet(pAssembly->EnumerateModules(&modules));

            ICorDebugModule *curModule;
            ULONG modulesFetched;
            while (SUCCEEDED(modules->Next(1, &curModule, &modulesFetched)) && modulesFetched == 1)
            {
                ToRelease<ICorDebugModule> pModule(curModule);
                if (SUCCEEDED(ResolveBreakpoint(pModule, filename, linenum, bp)))
                {
                    std::lock_guard<std::mutex> lock(g_breakMutex);
                    id = g_breakIndex++;
                    bp.id = id;
                    g_breaks.insert(std::make_pair(id, std::move(bp)));
                    return S_OK;
                }
            }
        }
    }

    // Add pending breakpoint
    std::lock_guard<std::mutex> lock(g_breakMutex);
    id = g_breakIndex++;
    bp.id = id;
    g_breaks.insert(std::make_pair(id, std::move(bp)));

    return S_FALSE;
}