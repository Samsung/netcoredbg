// #include <iostream>
// #include <cstdlib>

#include <windows.h>
// #include <winver.h>
// #include <winternl.h>
// #include <psapi.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"
#include "clrinternal.h"

#include <unistd.h>
#include <sys/syscall.h>

#include <histedit.h>

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <fstream>

#include "symbolreader.h"

EXTERN_C HRESULT CreateDebuggingInterfaceFromVersionEx(
    int iDebuggerVersion,
    LPCWSTR szDebuggeeVersion,
    IUnknown ** ppCordb);

EXTERN_C HRESULT
CreateVersionStringFromModule(
    DWORD pidDebuggee,
    LPCWSTR szModuleName,
    LPWSTR pBuffer,
    DWORD cchBuffer,
    DWORD* pdwLength);

#include <arrayholder.h>
#include "torelease.h"

ICorDebugProcess *g_process = NULL;

ULONG OSPageSize ()
{
    static ULONG pageSize = 0;
    if (pageSize == 0)
        pageSize = sysconf(_SC_PAGESIZE);

    return pageSize;
}

size_t NextOSPageAddress (size_t addr)
{
    size_t pageSize = OSPageSize();
    return (addr+pageSize)&(~(pageSize-1));
}

/**********************************************************************\
* Routine Description:                                                 *
*                                                                      *
*    This function is called to read memory from the debugee's         *
*    address space.  If the initial read fails, it attempts to read    *
*    only up to the edge of the page containing "offset".              *
*                                                                      *
\**********************************************************************/
BOOL SafeReadMemory (TADDR offset, PVOID lpBuffer, ULONG cb,
                     PULONG lpcbBytesRead)
{
    BOOL bRet = FALSE;

    SIZE_T bytesRead = 0;

    bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
                                           &bytesRead));

    if (!bRet)
    {
        cb   = (ULONG)(NextOSPageAddress(offset) - offset);
        bRet = SUCCEEDED(g_process->ReadMemory(TO_CDADDR(offset), cb, (BYTE*)lpBuffer,
                                            &bytesRead));
    }

    *lpcbBytesRead = bytesRead;
    return bRet;
}

/* This holds all the state for our line editor */
EditLine *el;

const char * prompt(EditLine *e) {
    return "(gdb) ";
}

// Printing the following string causes libedit to back up to the beginning of the line & blank it out.
const char undo_prompt_string[4] = { (char) 13, (char) 27, (char) 91, (char) 75};

void _el_printf(EditLine *el, const char *fmt, ...)
    __attribute__((format (printf, 2, 3)));

#define el_printf(el, fmt, ...) _el_printf(el, fmt, ##__VA_ARGS__)

void _el_printf(EditLine *el, const char *fmt, ...)
{
    fwrite(undo_prompt_string , sizeof(char), sizeof(undo_prompt_string), stdout);
    va_list arg;

    /* Write the error message */
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);

    fflush(stdout);

    el_set(el, EL_REFRESH);
}

HRESULT PrintThread(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status = S_OK;

    std::stringstream ss;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));
    CorDebugThreadState state = THREAD_SUSPEND;
    IfFailRet(pThread->GetDebugState(&state));

    CorDebugUserState ustate;
    IfFailRet(pThread->GetUserState(&ustate));

    static const struct { int val; const char *name; } states[] = {
        { USER_STOP_REQUESTED, "USER_STOP_REQUESTED" },
        { USER_SUSPEND_REQUESTED, "USER_SUSPEND_REQUESTED" },
        { USER_BACKGROUND, "USER_BACKGROUND" },
        { USER_UNSTARTED, "USER_UNSTARTED" },
        { USER_STOPPED, "USER_STOPPED" },
        { USER_WAIT_SLEEP_JOIN, "USER_WAIT_SLEEP_JOIN" },
        { USER_SUSPENDED, "USER_SUSPENDED" },
        { USER_UNSAFE_POINT, "USER_UNSAFE_POINT" },
        { USER_THREADPOOL, "USER_THREADPOOL" }
    };

    std::string user_state;
    for (int i = 0; i < sizeof(states)/sizeof(states[0]); i++)
    {
        if (ustate & states[i].val)
        {
            if (!user_state.empty()) user_state += '|';
            user_state += states[i].name;
        }
    }

    ss << "{id=\"" << threadId
       << "\",name=\"<No name>\",state=\"" << (state == THREAD_RUN ? "running" : "stopped")
       << "-" << user_state << "\"}";

    output = ss.str();

    return S_OK;
}

HRESULT PrintThreadsState(ICorDebugController *controller, std::string &output)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugThreadEnum> pThreads;
    IfFailRet(controller->EnumerateThreads(&pThreads));

    std::stringstream ss;

    ss << "^done,threads=[";

    ICorDebugThread *handle;
    ULONG fetched;
    const char *sep = "";
    while (SUCCEEDED(Status = pThreads->Next(1, &handle, &fetched)) && fetched == 1)
    {
        ToRelease<ICorDebugThread> pThread = handle;

        std::string threadOutput;
        PrintThread(pThread, threadOutput);

        ss << sep << threadOutput;
        sep = ",";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

HRESULT PrintFrameLocation(ICorDebugFrame *pFrame, std::string &output)
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

    ULONG32 nOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&nOffset, &mappingResult));

    SymbolReader symbolReader;
    if (SUCCEEDED(symbolReader.LoadSymbols(pMDImport, pModule)))
    {
        WCHAR name[mdNameLen];
        ULONG linenum;
        IfFailRet(symbolReader.GetLineByILOffset(methodToken, nOffset, &linenum, name, mdNameLen));

        char cname[mdNameLen];

        WideCharToMultiByte(CP_UTF8, 0, name, (int)(PAL_wcslen(name) + 1), cname, mdNameLen, NULL, NULL);

        std::stringstream ss;
        ss << "line=\"" << linenum << "\",fullname=\"" << cname << "\"";
        output = ss.str();
    }

    return S_OK;
}
HRESULT PrintLocation(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));

    return PrintFrameLocation(pFrame, output);
}

HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));

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

    ULONG32 nOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&nOffset, &mappingResult));

    SymbolReader symbolReader;
    IfFailRet(symbolReader.LoadSymbols(pMDImport, pModule));

    ULONG32 ilStartOffset;
    ULONG32 ilEndOffset;

    IfFailRet(symbolReader.GetStepRangesFromIP(nOffset, methodToken, &ilStartOffset, &ilEndOffset));

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

struct ModuleInfo
{
    CORDB_ADDRESS address;
    std::shared_ptr<SymbolReader> symbols;
    //ICorDebugModule *module;
};

std::mutex g_modulesInfoMutex;
std::unordered_map<std::string, ModuleInfo> g_modulesInfo;

std::string GetModuleName(ICorDebugModule *pModule)
{
    char cname[mdNameLen];
    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;
    if (SUCCEEDED(pModule->GetName(mdNameLen, &name_len, name)))
    {
        WideCharToMultiByte(CP_UTF8, 0, name, (int)(PAL_wcslen(name) + 1), cname, mdNameLen, NULL, NULL);
        return cname;
    }
    return std::string();
}

HRESULT CreateBreakpoint(ICorDebugModule *pModule, std::string filename, int linenum)
{
    HRESULT Status;

    WCHAR nameBuffer[MAX_LONGPATH];

    Status = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), filename.size() + 1, nameBuffer, MAX_LONGPATH);

    std::string modName = GetModuleName(pModule);

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        auto info_pair = g_modulesInfo.find(modName);
        if (info_pair == g_modulesInfo.end())
        {
            return E_FAIL;
        }

        CORDB_ADDRESS modAddress;
        IfFailRet(pModule->GetBaseAddress(&modAddress));

        mdMethodDef methodToken;
        ULONG32 ilOffset;

        IfFailRet(info_pair->second.symbols->ResolveSequencePoint(nameBuffer, linenum, modAddress, &methodToken, &ilOffset));

        //el_printf(el, "  methodToken=0x%x ilOffset=%i\n", methodToken, ilOffset);

        ToRelease<ICorDebugFunction> pFunc;
        ToRelease<ICorDebugCode> pCode;
        IfFailRet(pModule->GetFunctionFromToken(methodToken, &pFunc));
        IfFailRet(pFunc->GetILCode(&pCode));

        ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
        IfFailRet(pCode->CreateBreakpoint(ilOffset, &pBreakpoint));
        IfFailRet(pBreakpoint->Activate(TRUE));

        return S_OK;
    }

    return E_FAIL;
}

HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain = curDomain;

        ToRelease<ICorDebugAssemblyEnum> assemblies;
        IfFailRet(pDomain->EnumerateAssemblies(&assemblies));

        ICorDebugAssembly *curAssembly;
        ULONG assembliesFetched;
        while (SUCCEEDED(assemblies->Next(1, &curAssembly, &assembliesFetched)) && assembliesFetched == 1)
        {
            ToRelease<ICorDebugAssembly> pAssembly = curAssembly;

            ToRelease<ICorDebugModuleEnum> modules;
            IfFailRet(pAssembly->EnumerateModules(&modules));

            ICorDebugModule *curModule;
            ULONG modulesFetched;
            while (SUCCEEDED(modules->Next(1, &curModule, &modulesFetched)) && modulesFetched == 1)
            {
                ToRelease<ICorDebugModule> pModule = curModule;

                if (SUCCEEDED(CreateBreakpoint(pModule, filename, linenum)))
                    return S_OK;
            }
        }
    }
    return S_FALSE;
}

HRESULT DisableAllBreakpointsAndSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;

    ToRelease<ICorDebugBreakpointEnum> breakpoints;
    if (SUCCEEDED(pAppDomain->EnumerateBreakpoints(&breakpoints)))
    {
        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint = curBreakpoint;
            pBreakpoint->Activate(FALSE);
        }
    }

    ToRelease<ICorDebugStepperEnum> steppers;
    if (SUCCEEDED(pAppDomain->EnumerateSteppers(&steppers)))
    {
        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            ToRelease<ICorDebugStepper> pStepper = curStepper;
            pStepper->Deactivate();
        }
    }

    return S_OK;
}

HRESULT DisableAllBreakpointsAndSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain = curDomain;
        DisableAllBreakpointsAndSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

HRESULT TryLoadModuleSymbols(ICorDebugModule *pModule)
{
    HRESULT Status;
    std::string name = GetModuleName(pModule);

    if (name.empty())
        return E_FAIL;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    auto symbolReader = std::make_shared<SymbolReader>();
    IfFailRet(symbolReader->LoadSymbols(pMDImport, pModule));

    CORDB_ADDRESS modAddress;
    pModule->GetBaseAddress(&modAddress);

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        g_modulesInfo.insert({name, {modAddress, symbolReader}});
    }
    return S_OK;
}

enum StepType {
    STEP_IN = 0,
    STEP_OVER,
    STEP_OUT
};

HRESULT RunStep(ICorDebugThread *pThread, StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());
        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    return S_OK;
}

static HRESULT NameForTypeDef_s(mdTypeDef tkTypeDef, IMetaDataImport *pImport,
                                WCHAR *mdName, size_t capacity_mdName)
{
    DWORD flags;
    ULONG nameLen;

    HRESULT hr = pImport->GetTypeDefProps(tkTypeDef, mdName,
                                          mdNameLen, &nameLen,
                                          &flags, NULL);
    if (hr != S_OK) {
        return hr;
    }

    if (!IsTdNested(flags)) {
        return hr;
    }
    mdTypeDef tkEnclosingClass;
    hr = pImport->GetNestedClassProps(tkTypeDef, &tkEnclosingClass);
    if (hr != S_OK) {
        return hr;
    }
    WCHAR *name = (WCHAR*)_alloca((nameLen+1)*sizeof(WCHAR));
    wcscpy_s (name, nameLen+1, mdName);
    hr = NameForTypeDef_s(tkEnclosingClass,pImport,mdName, capacity_mdName);
    if (hr != S_OK) {
        return hr;
    }
    size_t Len = _wcslen (mdName);
    if (Len < mdNameLen-2) {
        mdName[Len++] = L'+';
        mdName[Len] = L'\0';
    }
    Len = mdNameLen-1 - Len;
    if (Len > nameLen) {
        Len = nameLen;
    }
    wcsncat_s (mdName,capacity_mdName,name,Len);
    return hr;
}

HRESULT PrintFrames(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status;
    std::stringstream ss;

    ToRelease<ICorDebugThread3> pThread3;
    ToRelease<ICorDebugStackWalk> pStackWalk;

    IfFailRet(pThread->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &pThread3));
    IfFailRet(pThread3->CreateStackWalk(&pStackWalk));

    int currentFrame = -1;

    ss << "stack=[";

    for (Status = S_OK; ; Status = pStackWalk->Next())
    {
        currentFrame++;

        if (Status == CORDBG_S_AT_END_OF_STACK)
            break;

        IfFailRet(Status);

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pStackWalk->GetFrame(&pFrame));
        if (Status == S_FALSE)
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[NativeStackFrame]\"}";
            continue;
        }

        ToRelease<ICorDebugRuntimeUnwindableFrame> pRuntimeUnwindableFrame;
        Status = pFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &pRuntimeUnwindableFrame);
        if (SUCCEEDED(Status))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[RuntimeUnwindableFrame]\"}";
            continue;
        }

        ToRelease<ICorDebugILFrame> pILFrame;
        HRESULT hrILFrame = pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame);

        if (FAILED(hrILFrame))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"?\"}";
            continue;
        }

        ToRelease<ICorDebugFunction> pFunction;
        Status = pFrame->GetFunction(&pFunction);
        if (FAILED(Status))
        {
            ss << (currentFrame != 0 ? "," : "");
            ss << "frame={level=\"" << currentFrame << "\",func=\"[IL Stub or LCG]\"}";
            continue;
        }

        std::string frameLocation;
        PrintFrameLocation(pFrame, frameLocation);

        ss << (currentFrame != 0 ? "," : "");
        ss << "frame={level=\"" << currentFrame << "\",";
        if (!frameLocation.empty())
            ss << frameLocation << ",";

        ToRelease<ICorDebugClass> pClass;
        ToRelease<ICorDebugModule> pModule;
        mdMethodDef methodDef;
        IfFailRet(pFunction->GetClass(&pClass));
        IfFailRet(pFunction->GetModule(&pModule));
        IfFailRet(pFunction->GetToken(&methodDef));

        WCHAR wszModuleName[100];
        ULONG32 cchModuleNameActual;
        IfFailRet(pModule->GetName(_countof(wszModuleName), &cchModuleNameActual, wszModuleName));

        ToRelease<IUnknown> pMDUnknown;
        ToRelease<IMetaDataImport> pMD;

        IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
        IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

        mdTypeDef typeDef;
        IfFailRet(pClass->GetToken(&typeDef));

        HRESULT hr;
        mdTypeDef memTypeDef;
        ULONG nameLen;
        DWORD flags;
        PCCOR_SIGNATURE pbSigBlob;
        ULONG ulSigBlob;
        ULONG ulCodeRVA;
        ULONG ulImplFlags;

        WCHAR szFunctionName[1024] = {0};

        hr = pMD->GetMethodProps(methodDef, &memTypeDef,
                                 szFunctionName, _countof(szFunctionName), &nameLen,
                                 &flags, &pbSigBlob, &ulSigBlob, &ulCodeRVA, &ulImplFlags);
        szFunctionName[nameLen] = L'\0';
        WCHAR m_szName[mdNameLen] = {0};
        m_szName[0] = L'\0';

        if (memTypeDef != mdTypeDefNil)
        {
            hr = NameForTypeDef_s (memTypeDef, pMD, m_szName, _countof(m_szName));
            if (SUCCEEDED (hr)) {
                wcscat_s (m_szName, _countof(m_szName), W("."));
            }
        }
        wcscat_s (m_szName, _countof(m_szName), szFunctionName);

        // TODO:
        // LONG lSigBlobRemaining;
        // hr = GetFullNameForMD(pbSigBlob, ulSigBlob, &lSigBlobRemaining);

        char funcName[2048] = {0};
        WideCharToMultiByte(CP_UTF8, 0, m_szName, (int)(_wcslen(m_szName) + 1), funcName, _countof(funcName), NULL, NULL);

        ss << "func=\"" << funcName << "\"}";
    }

    ss << "]";

    output = ss.str();

    return S_OK;
}

std::mutex g_currentThreadMutex;
ICorDebugThread *g_currentThread = nullptr;

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2
{
    ULONG m_refCount;
public:

        void HandleEvent(ICorDebugController *controller, const char *eventName)
        {
            el_printf(el, "test");
            el_printf(el, "event received on tid %li: %s\n", syscall(SYS_gettid), eventName);
            controller->Continue(0);
        }

        ManagedCallback() : m_refCount(1) {}
        virtual ~ManagedCallback() {}

        // IUnknown

        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppInterface)
        {
            if(riid == __uuidof(ICorDebugManagedCallback))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(ICorDebugManagedCallback2))
            {
                *ppInterface = static_cast<ICorDebugManagedCallback2*>(this);
                AddRef();
                return S_OK;
            }
            else if(riid == __uuidof(IUnknown))
            {
                *ppInterface = static_cast<IUnknown*>(static_cast<ICorDebugManagedCallback*>(this));
                AddRef();
                return S_OK;
            }
            else
            {
                return E_NOINTERFACE;
            }
        }

        virtual ULONG STDMETHODCALLTYPE AddRef()
        {
            return InterlockedIncrement((volatile LONG *) &m_refCount);
        }

        virtual ULONG STDMETHODCALLTYPE Release()
        {
            ULONG count = InterlockedDecrement((volatile LONG *) &m_refCount);
            if(count == 0)
            {
                delete this;
            }
            return count;
        }

        // ICorDebugManagedCallback

        virtual HRESULT STDMETHODCALLTYPE Breakpoint(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint)
        {
            std::string output;
            PrintLocation(pThread, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            el_printf(el, "*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"1\",%s\n",
                (int)threadId, output.c_str());
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                if (g_currentThread)
                    g_currentThread->Release();
                pThread->AddRef();
                g_currentThread = pThread;
            }
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE StepComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugStepper *pStepper,
            /* [in] */ CorDebugStepReason reason)
        {
            std::string output;
            PrintLocation(pThread, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            el_printf(el, "*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",%s\n",
                (int)threadId, output.c_str());

            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                if (g_currentThread)
                    g_currentThread->Release();
                pThread->AddRef();
                g_currentThread = pThread;
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread) { HandleEvent(pAppDomain, "Break"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ BOOL unhandled) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EvalComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EvalException(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            //HandleEvent(pProcess, "CreateProcess");
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitProcess(
            /* [in] */ ICorDebugProcess *pProcess)
        {
            el_printf(el, "*stopped,reason=\"exited\",exit-code=\"%i\"\n", 0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            DWORD threadId = 0;
            thread->GetID(&threadId);
            el_printf(el, "=thread-created,id=\"%i\"\n", (int)threadId);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            HandleEvent(pAppDomain, "ExitThread");
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE LoadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule)
        {
            std::string name = GetModuleName(pModule);
            if (!name.empty())
            {
                el_printf(el, "=library-loaded,target-name=\"%s\"\n", name.c_str());
            }
            TryLoadModuleSymbols(pModule);
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadModule(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule) { HandleEvent(pAppDomain, "UnloadModule"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "LoadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE UnloadClass(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugClass *c) { HandleEvent(pAppDomain, "UnloadClass"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DebuggerError(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ HRESULT errorHR,
            /* [in] */ DWORD errorCode) { printf("DebuggerError\n"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogMessage(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pMessage) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LogSwitch(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ LONG lLevel,
            /* [in] */ ULONG ulReason,
            /* [in] */ WCHAR *pLogSwitchName,
            /* [in] */ WCHAR *pParentName) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain)
        {
            //HandleEvent(pProcess, "CreateAppDomain");
            pProcess->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE ExitAppDomain(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ ICorDebugAppDomain *pAppDomain) { HandleEvent(pAppDomain, "ExitAppDomain"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE LoadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly)
        {
            //HandleEvent(pAppDomain, "LoadAssembly");
            pAppDomain->Continue(0);
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE UnloadAssembly(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugAssembly *pAssembly) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ControlCTrap(
            /* [in] */ ICorDebugProcess *pProcess) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE NameChange(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE UpdateModuleSymbols(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugModule *pModule,
            /* [in] */ IStream *pSymbolStream) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE EditAndContinueRemap(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction,
            /* [in] */ BOOL fAccurate) { return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE BreakpointSetError(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugBreakpoint *pBreakpoint,
            /* [in] */ DWORD dwError) {return S_OK; }


        // ICorDebugManagedCallback2

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapOpportunity(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pOldFunction,
            /* [in] */ ICorDebugFunction *pNewFunction,
            /* [in] */ ULONG32 oldILOffset) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE CreateConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId,
            /* [in] */ WCHAR *pConnName) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ChangeConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE DestroyConnection(
            /* [in] */ ICorDebugProcess *pProcess,
            /* [in] */ CONNID dwConnectionId) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFrame *pFrame,
            /* [in] */ ULONG32 nOffset,
            /* [in] */ CorDebugExceptionCallbackType dwEventType,
            /* [in] */ DWORD dwFlags) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE ExceptionUnwind(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ CorDebugExceptionUnwindCallbackType dwEventType,
            /* [in] */ DWORD dwFlags) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE FunctionRemapComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugFunction *pFunction) {return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE MDANotification(
            /* [in] */ ICorDebugController *pController,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugMDA *pMDA) {return S_OK; }
};

std::string GetCoreCLRLPath(int pid)
{
    static const char *coreclr_so = "/libcoreclr.so";
    static const std::size_t coreclr_so_len = strlen(coreclr_so);

    char maps_name[100];
    snprintf(maps_name, sizeof(maps_name), "/proc/%i/maps", pid);
    std::ifstream input(maps_name);

    for(std::string line; std::getline(input, line); )
    {
        std::size_t i = line.rfind(coreclr_so);
        if (i == std::string::npos)
            continue;
        if (i + coreclr_so_len != line.size())
            continue;
        std::size_t si = line.rfind(' ', i);
        if (i == std::string::npos)
            continue;
        return line.substr(si + 1);//, i - si - 1);
    }
    return std::string();
}

void print_help()
{
    fprintf(stderr,
        "CoreCLR debugger for Linux.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n");
}

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        print_help();
        return EXIT_FAILURE;
    }

    DWORD pidDebuggee = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--attach") == 0)
        {
            i++;
            if (i >= argc)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
            char *err;
            pidDebuggee = strtoul(argv[i], &err, 10);
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "--interpreter=mi") == 0)
        {
            continue;
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            print_help();
            return EXIT_SUCCESS;
        }
        else
        {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (pidDebuggee == 0)
    {
        fprintf(stderr, "Error: Missing process id\n");
        return EXIT_FAILURE;
    }

    std::string coreclrPath = GetCoreCLRLPath(pidDebuggee);
    if (coreclrPath.empty())
    {
        fprintf(stderr, "Error: Unable to find libcoreclr.so\n");
        return EXIT_FAILURE;
    }

    SymbolReader::SetCoreCLRPath(coreclrPath);

    WCHAR szModuleName[MAX_LONGPATH];
    MultiByteToWideChar(CP_UTF8, 0, coreclrPath.c_str(), coreclrPath.size() + 1, szModuleName, MAX_LONGPATH);

    WCHAR pBuffer[100];
    DWORD dwLength;
    HRESULT hr = CreateVersionStringFromModule(
        pidDebuggee,
        szModuleName,
        pBuffer,
        100,
        &dwLength);

    if (FAILED(hr))
    {
        fprintf(stderr, "CreateVersionStringFromModule failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    ToRelease<IUnknown> pCordb;
    //WCHAR szDebuggeeVersion[] = W("4.0");
    hr = CreateDebuggingInterfaceFromVersionEx(4, pBuffer, &pCordb);

    if (FAILED(hr))
    {
        fprintf(stderr, "CreateDebuggingInterfaceFromVersionEx failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    ToRelease<ICorDebug> pCorDebug;

    hr = pCordb->QueryInterface(IID_ICorDebug, (LPVOID *)&pCorDebug);
    if (FAILED(hr))
    {
        fprintf(stderr, "QueryInterface(IID_ICorDebug) failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->Initialize();
    if (FAILED(hr))
    {
        fprintf(stderr, "Initialize failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->SetManagedHandler(new ManagedCallback());
    if (FAILED(hr))
    {
        fprintf(stderr, "SetManagedHandler failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    hr = pCorDebug->CanLaunchOrAttach(pidDebuggee, FALSE);
    //fprintf(stderr, "CanLaunchOrAttach : hr=%x\n", hr);

    ToRelease<ICorDebugProcess> pProcess;
    hr = pCorDebug->DebugActiveProcess(
            pidDebuggee,
            FALSE,
            &pProcess);
    if (FAILED(hr))
    {
        fprintf(stderr, "DebugActiveProcess failed: hr=%x\n", hr);
        return EXIT_FAILURE;
    }

    g_process = pProcess;

    //printf("libedit thread %li\n", syscall(SYS_gettid));


    /* This holds the info for our history */
    History *myhistory;

    /* Temp variables */
    int keepreading = 1;
    HistEvent ev;

    /* Initialize the EditLine state to use our prompt function and
    emacs style editing. */

    el = el_init(argv[0], stdin, stdout, stderr);
    el_set(el, EL_PROMPT, &prompt);
    el_set(el, EL_EDITOR, "emacs");
    el_set(el, EL_SIGNAL, 1);

    /* Initialize the history */
    myhistory = history_init();
    if (myhistory == 0) {
        fprintf(stderr, "history could not be initialized\n");
        return 1;
    }

    /* Set the size of the history */
    history(myhistory, &ev, H_SETSIZE, 800);

    /* This sets up the call back functions for history functionality */
    el_set(el, EL_HIST, history, myhistory);

    std::string prev_command;

    while (keepreading) {
        /* count is the number of characters read.
           line is a const char* of our command line with the tailing \n */
        int count;

        const char *raw_line = el_gets(el, &count);
        // Sleep(3000);
        // const char *raw_line = "-gdb-exit\n";
        // count = strlen(raw_line);

        if (count <= 0)
        {
            keepreading = 0;
            break;
        }

        /* In order to use our history we have to explicitly add commands
        to the history */

        std::string line(raw_line, count - 1);

        if (!line.empty())
        {
            history(myhistory, &ev, H_ENTER, raw_line);
            prev_command = line;
        }
        else
        {
            line = prev_command;
        }

        if (line == "-thread-info")
        {
            std::string output;
            HRESULT hr = PrintThreadsState(pProcess, output);
            printf("%x:%s\n", hr, output.c_str());
        }
        else if (line == "-exec-continue")
        {
            HRESULT hr = pProcess->Continue(0);
            if (SUCCEEDED(hr))
            {
                printf("^done\n");
            }
            else
            {
                printf("^error,msg=\"HRESULT=%x\"\n", hr);
            }
        }
        else if (line == "-exec-interrupt")
        {
            HRESULT hr = pProcess->Stop(0);
            if (SUCCEEDED(hr))
            {
                printf("^done\n");
            }
            else
            {
                printf("^error,msg=\"HRESULT=%x\"\n", hr);
            }
        }
        else if (line.find("-break-insert ") == 0)
        {
            // TODO: imlement proper argument parsing
            std::size_t i1 = line.find(' ');
            std::size_t i2 = line.rfind(':');

            if (i1 != std::string::npos && i2 != std::string::npos)
            {
                std::string filename = line.substr(i1 + 1, i2 - i1 - 1);
                std::string slinenum = line.substr(i2 + 1);

                int linenum = std::stoi(slinenum);
                if (CreateBreakpointInProcess(pProcess, filename, linenum) == S_OK)
                {
                    int bkpt_num = 1;
                    printf("^done,bkpt={number=\"%i\",fullname=\"%s\",line=\"%i\"}\n", bkpt_num, filename.c_str(), linenum);
                }
            }
            else
            {
                printf("^error,msg=\"Unknown breakpoint location format\"\n");
            }
        }
        else if (line == "-exec-next" || line == "-exec-step" || line == "-exec-finish")
        {
            StepType stepType;
            if (line == "-exec-next")
                stepType = STEP_OVER;
            else if (line == "-exec-step")
                stepType = STEP_IN;
            else if (line == "-exec-finish")
                stepType = STEP_OUT;

            HRESULT hr;
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                hr = g_currentThread ? RunStep(g_currentThread, stepType) : E_FAIL;
            }

            if (FAILED(hr))
            {
                printf("^error,msg=\"Cannot create stepper: %x\"\n", hr);
            }
            else
            {
                hr = pProcess->Continue(0);
                if (SUCCEEDED(hr))
                {
                    printf("^done\n");
                }
                else
                {
                    printf("^error,msg=\"HRESULT=%x\"\n", hr);
                }
            }
        }
        else if (line == "-stack-list-frames")
        {
            std::string output;
            HRESULT hr;
            {
                std::lock_guard<std::mutex> lock(g_currentThreadMutex);
                hr = PrintFrames(g_currentThread, output);
            }
            if (SUCCEEDED(hr))
            {
                printf("^done,%s\n", output.c_str());
            }
            else
            {
                printf("^error,msg=\"HRESULT=%x\"\n", hr);
            }
        }
        else if (line == "-gdb-exit")
        {
            hr = pProcess->Stop(0);
            if (SUCCEEDED(hr))
            {
                DisableAllBreakpointsAndSteppers(pProcess);

                hr = pProcess->Terminate(0);

                // TODO: wait for process exit event
                Sleep(2000);

                pProcess.Release();
            }
            keepreading = 0;
        } else {
            printf("^error,msg=\"Unknown command: %s\"\n", line.c_str());
        }
    }

    /* Clean up our memory */
    history_end(myhistory);
    el_end(el);

    if (pProcess)
    {
        hr = pProcess->Stop(0);
        //fprintf(stderr, "Stop : hr=%x\n", hr);
        if (SUCCEEDED(hr))
        {
            DisableAllBreakpointsAndSteppers(pProcess);
            hr = pProcess->Detach();
        }
    }
    //fprintf(stderr, "Detach : hr=%x\n", hr);

    pCorDebug->Terminate();

    printf("^exit\n");

    // TODO: Cleanup libcoreclr.so instance

    return EXIT_SUCCESS;
}
