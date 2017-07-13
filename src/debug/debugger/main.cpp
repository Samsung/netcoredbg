#include "common.h"

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <vector>
#include <list>

#include "platform.h"
#include "typeprinter.h"


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

static std::mutex g_processMutex;
static std::condition_variable g_processCV;
static ICorDebugProcess *g_process = nullptr;

static void NotifyProcessExited()
{
    std::lock_guard<std::mutex> lock(g_processMutex);
    g_process = nullptr;
    g_processMutex.unlock();
    g_processCV.notify_one();
}

void WaitProcessExited()
{
    std::unique_lock<std::mutex> lock(g_processMutex);
    if (g_process)
        g_processCV.wait(lock, []{return g_process == nullptr;});
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
    std::lock_guard<std::mutex> lock(g_processMutex);

    if (!g_process)
        return FALSE;

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

std::mutex g_outMutex;

void _out_printf(const char *fmt, ...)
    __attribute__((format (printf, 1, 2)));

#define out_printf(fmt, ...) _out_printf(fmt, ##__VA_ARGS__)

void _out_printf(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_outMutex);
    va_list arg;

    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);

    fflush(stdout);
}

// Breakpoints
void DeleteAllBreakpoints();
HRESULT FindCurrentBreakpointId(ICorDebugThread *pThread, ULONG32 &id);
HRESULT TryResolveBreakpointsForModule(ICorDebugModule *pModule);

// Modules
void SetCoreCLRPath(const std::string &coreclrPath);
std::string GetModuleName(ICorDebugModule *pModule);
HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range);
HRESULT TryLoadModuleSymbols(ICorDebugModule *pModule,
                             std::string &id,
                             std::string &name,
                             bool &symbolsLoaded,
                             CORDB_ADDRESS &baseAddress,
                             ULONG32 &size);


// Valuewalk
void NotifyEvalComplete();

// Frames
HRESULT PrintFrameLocation(ICorDebugFrame *pFrame, std::string &output);

static HRESULT DisableAllBreakpointsAndSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;

    ToRelease<ICorDebugBreakpointEnum> breakpoints;
    if (SUCCEEDED(pAppDomain->EnumerateBreakpoints(&breakpoints)))
    {
        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
            pBreakpoint->Activate(FALSE);
        }
    }

    DeleteAllBreakpoints();

    ToRelease<ICorDebugStepperEnum> steppers;
    if (SUCCEEDED(pAppDomain->EnumerateSteppers(&steppers)))
    {
        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            ToRelease<ICorDebugStepper> pStepper(curStepper);
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
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllBreakpointsAndSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

static std::mutex g_lastStoppedThreadIdMutex;
static int g_lastStoppedThreadId = 0;

void SetLastStoppedThread(ICorDebugThread *pThread)
{
    DWORD threadId = 0;
    pThread->GetID(&threadId);

    std::lock_guard<std::mutex> lock(g_lastStoppedThreadIdMutex);
    g_lastStoppedThreadId = threadId;
}

int GetLastStoppedThreadId()
{
    std::lock_guard<std::mutex> lock(g_lastStoppedThreadIdMutex);
    return g_lastStoppedThreadId;
}

class ManagedCallback : public ICorDebugManagedCallback, ICorDebugManagedCallback2
{
    ULONG m_refCount;
public:

        void HandleEvent(ICorDebugController *controller, const char *eventName)
        {
            out_printf("=message,text=\"event received %s\"\n", eventName);
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
            ULONG32 id = 0;
            FindCurrentBreakpointId(pThread, id);

            std::string output;
            ToRelease<ICorDebugFrame> pFrame;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)))
                PrintFrameLocation(pFrame, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            out_printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",%s\n",
                (int)threadId, (unsigned int)id, output.c_str());

            SetLastStoppedThread(pThread);

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE StepComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugStepper *pStepper,
            /* [in] */ CorDebugStepReason reason)
        {
            std::string output;
            ToRelease<ICorDebugFrame> pFrame;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)))
                PrintFrameLocation(pFrame, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);

            out_printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",%s\n",
                (int)threadId, output.c_str());

            SetLastStoppedThread(pThread);

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE Break(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread) { HandleEvent(pAppDomain, "Break"); return S_OK; }

        virtual HRESULT STDMETHODCALLTYPE Exception(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ BOOL unhandled)
        {
            std::string output;
            ToRelease<ICorDebugFrame> pFrame;
            if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)))
                PrintFrameLocation(pFrame, output);

            DWORD threadId = 0;
            pThread->GetID(&threadId);
            SetLastStoppedThread(pThread);

            if (unhandled)
            {
                out_printf("*stopped,reason=\"exception-received\",exception-stage=\"%s\",thread-id=\"%i\",stopped-threads=\"all\",%s\n",
                    unhandled ? "unhandled" : "handled", (int)threadId, output.c_str());
            } else {
                // TODO: Add exception name and module
                out_printf("=message,text=\"Exception thrown: '%s' in %s\\n\",send-to=\"output-window\",source=\"target-exception\"\n",
                    "<exceptions.name>", "<short.module.name>");
                pAppDomain->Continue(0);
            }

            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalComplete(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            NotifyEvalComplete();
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE EvalException(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *pThread,
            /* [in] */ ICorDebugEval *pEval)
        {
            NotifyEvalComplete();
            return S_OK;
        }

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
            out_printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", 0);
            NotifyEvalComplete();
            NotifyProcessExited();
            return S_OK;
        }

        virtual HRESULT STDMETHODCALLTYPE CreateThread(
            /* [in] */ ICorDebugAppDomain *pAppDomain,
            /* [in] */ ICorDebugThread *thread)
        {
            DWORD threadId = 0;
            thread->GetID(&threadId);
            out_printf("=thread-created,id=\"%i\"\n", (int)threadId);
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
            std::string id;
            std::string name;
            bool symbolsLoaded = false;
            CORDB_ADDRESS baseAddress = 0;
            ULONG32 size = 0;

            TryLoadModuleSymbols(pModule, id, name, symbolsLoaded, baseAddress, size);
            {
                std::stringstream ss;
                ss << "id=\"{" << id << "}\",target-name=\"" << name << "\","
                   << "symbols-loaded=\"" << symbolsLoaded << "\","
                   << "base-address=\"0x" << std::hex << baseAddress << "\","
                   << "size=\"" << std::dec << size << "\"";
                out_printf("=library-loaded,%s\n", ss.str().c_str());
            }
            if (symbolsLoaded)
                TryResolveBreakpointsForModule(pModule);

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
            /* [in] */ DWORD dwFlags)
        {
            // const char *cbTypeName;
            // switch(dwEventType)
            // {
            //     case DEBUG_EXCEPTION_FIRST_CHANCE: cbTypeName = "FIRST_CHANCE"; break;
            //     case DEBUG_EXCEPTION_USER_FIRST_CHANCE: cbTypeName = "USER_FIRST_CHANCE"; break;
            //     case DEBUG_EXCEPTION_CATCH_HANDLER_FOUND: cbTypeName = "CATCH_HANDLER_FOUND"; break;
            //     case DEBUG_EXCEPTION_UNHANDLED: cbTypeName = "UNHANDLED"; break;
            //     default: cbTypeName = "?"; break;
            // }
            // out_printf("*stopped,reason=\"exception-received2\",exception-stage=\"%s\"\n",
            //     cbTypeName);
            pAppDomain->Continue(0);
            return S_OK;
        }

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

void CommandLoop(ICorDebugProcess *pProcess);

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

    std::string coreclrPath = GetCoreCLRPath(pidDebuggee);
    if (coreclrPath.empty())
    {
        fprintf(stderr, "Error: Unable to find libcoreclr.so\n");
        return EXIT_FAILURE;
    }

    SetCoreCLRPath(coreclrPath);

    WCHAR szModuleName[MAX_LONGPATH];
    MultiByteToWideChar(CP_UTF8, 0, coreclrPath.c_str(), coreclrPath.size() + 1, szModuleName, MAX_LONGPATH);

    WCHAR pBuffer[100];
    DWORD dwLength;
    HRESULT hr = CreateVersionStringFromModule(
        pidDebuggee,
        szModuleName,
        pBuffer,
        _countof(pBuffer),
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

    {
        std::lock_guard<std::mutex> lock(g_processMutex);
        g_process = pProcess;
    }

    CommandLoop(pProcess);

    pCorDebug->Terminate();

    // TODO: Cleanup libcoreclr.so instance

    return EXIT_SUCCESS;
}
