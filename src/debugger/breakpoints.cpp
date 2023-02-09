// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoint_break.h"
#include "debugger/breakpoint_entry.h"
#include "debugger/breakpoints_exception.h"
#include "debugger/breakpoints_func.h"
#include "debugger/breakpoints_line.h"
#include "debugger/breakpoint_hotreload.h"
#include "debugger/breakpoint_interop_rendezvous.h"
#include "debugger/breakpoints_interop.h"
#include "debugger/breakpoints_interop_line.h"
#include "debugger/breakpoints.h"
#include "debugger/breakpointutils.h"
#include "debugger/interop_brk_helpers.h"

#include <mutex>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <sstream>
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "utils/logger.h"
#include "utils/utf.h"
#include "managed/interop.h"
#include "utils/filesystem.h"

#include <palclr.h>

namespace netcoredbg
{

Breakpoints::Breakpoints(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Evaluator> &sharedEvaluator, std::shared_ptr<EvalHelpers> &sharedEvalHelpers, std::shared_ptr<Variables> &sharedVariables) :
        m_uniqueBreakBreakpoint(new BreakBreakpoint(sharedModules)),
        m_uniqueEntryBreakpoint(new EntryBreakpoint(sharedModules)),
        m_uniqueExceptionBreakpoints(new ExceptionBreakpoints(sharedEvaluator)),
        m_uniqueFuncBreakpoints(new FuncBreakpoints(sharedModules, sharedVariables)),
        m_uniqueLineBreakpoints(new LineBreakpoints(sharedModules, sharedVariables)),
        m_uniqueHotReloadBreakpoint(new HotReloadBreakpoint(sharedModules, sharedEvaluator, sharedEvalHelpers)),
#ifdef INTEROP_DEBUGGING
        m_sharedInteropBreakpoints(new InteropDebugging::InteropBreakpoints()),
        m_uniqueInteropRendezvousBreakpoint(new InteropDebugging::InteropRendezvousBreakpoint(m_sharedInteropBreakpoints)),
        m_sharedInteropLineBreakpoints(new InteropDebugging::InteropLineBreakpoints(m_sharedInteropBreakpoints)),
#endif // INTEROP_DEBUGGING
        m_nextBreakpointId(1)
    {}

void Breakpoints::SetJustMyCode(bool enable)
{
    m_uniqueFuncBreakpoints->SetJustMyCode(enable);
    m_uniqueLineBreakpoints->SetJustMyCode(enable);
    m_uniqueExceptionBreakpoints->SetJustMyCode(enable);
}

void Breakpoints::SetLastStoppedIlOffset(ICorDebugProcess *pProcess, const ThreadId &lastStoppedThreadId)
{
    m_uniqueBreakBreakpoint->SetLastStoppedIlOffset(pProcess, lastStoppedThreadId);
}

void Breakpoints::SetStopAtEntry(bool enable)
{
    m_uniqueEntryBreakpoint->SetStopAtEntry(enable);
}

HRESULT Breakpoints::ManagedCallbackBreak(ICorDebugThread *pThread, const ThreadId &lastStoppedThreadId)
{
    return m_uniqueBreakBreakpoint->ManagedCallbackBreak(pThread, lastStoppedThreadId);
}

void Breakpoints::DeleteAllManaged()
{
    m_uniqueEntryBreakpoint->Delete();
    m_uniqueFuncBreakpoints->DeleteAll();
    m_uniqueLineBreakpoints->DeleteAll();
    m_uniqueExceptionBreakpoints->DeleteAll();
    m_uniqueHotReloadBreakpoint->Delete();
}

HRESULT Breakpoints::DisableAllManaged(ICorDebugProcess *pProcess)
{
    HRESULT Status;
    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        ToRelease<ICorDebugBreakpointEnum> breakpoints;
        if (FAILED(pDomain->EnumerateBreakpoints(&breakpoints)))
            continue;

        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
            pBreakpoint->Activate(FALSE);
        }
    }

    return S_OK;
}

HRESULT Breakpoints::SetFuncBreakpoints(bool haveProcess, const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    return m_uniqueFuncBreakpoints->SetFuncBreakpoints(haveProcess, funcBreakpoints, breakpoints, [&]() -> uint32_t
    {
        std::lock_guard<std::mutex> lock(m_nextBreakpointIdMutex);
        return m_nextBreakpointId++;
    });
}

HRESULT Breakpoints::UpdateLineBreakpoint(bool haveProcess, int id, int linenum, Breakpoint &breakpoint)
{
    return m_uniqueLineBreakpoints->UpdateLineBreakpoint(haveProcess, id, linenum, breakpoint);
}

HRESULT Breakpoints::SetLineBreakpoints(bool haveProcess, const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    return m_uniqueLineBreakpoints->SetLineBreakpoints(haveProcess, filename, lineBreakpoints, breakpoints, [&]() -> uint32_t
    {
        std::lock_guard<std::mutex> lock(m_nextBreakpointIdMutex);
        return m_nextBreakpointId++;
    });
}

HRESULT Breakpoints::SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    return m_uniqueExceptionBreakpoints->SetExceptionBreakpoints(exceptionBreakpoints, breakpoints, [&]() -> uint32_t
    {
        std::lock_guard<std::mutex> lock(m_nextBreakpointIdMutex);
        return m_nextBreakpointId++;
    });
}

HRESULT Breakpoints::UpdateBreakpointsOnHotReload(ICorDebugModule *pModule, std::unordered_set<mdMethodDef> &methodTokens, std::vector<BreakpointEvent> &events)
{
    m_uniqueFuncBreakpoints->UpdateBreakpointsOnHotReload(pModule, methodTokens, events);
    m_uniqueLineBreakpoints->UpdateBreakpointsOnHotReload(pModule, methodTokens, events);
    return S_OK;
}

HRESULT Breakpoints::GetExceptionInfo(ICorDebugThread *pThread, ExceptionInfo &exceptionInfo)
{
    return m_uniqueExceptionBreakpoints->GetExceptionInfo(pThread, exceptionInfo);
}

HRESULT Breakpoints::ManagedCallbackBreakpoint(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint, bool &atEntry)
{
    // CheckBreakpointHit return:
    //     S_OK - breakpoint hit
    //     S_FALSE - no breakpoint hit.
    // ManagedCallbackBreakpoint return:
    //     S_OK - callback should be interrupted without event emit
    //     S_FALSE - callback should not be interrupted and emit stop event

    HRESULT Status;
    atEntry = false;
    if (SUCCEEDED(Status = m_uniqueEntryBreakpoint->CheckBreakpointHit(pThread, pBreakpoint)) &&
        Status == S_OK) // S_FALSE - no breakpoint hit
    {
        atEntry = true;
        return S_FALSE; // S_FALSE - not affect on callback (callback will emit stop event)
    }

    // Don't stop at breakpoint in not JMC code, if possible (error here is not fatal for debug process).
    // We need this check here, since we can't guarantee this check in SkipBreakpoint().
    ToRelease<ICorDebugFrame> iCorFrame;
    ToRelease<ICorDebugFunction> iCorFunction;
    ToRelease<ICorDebugFunction2> iCorFunction2;
    BOOL JMCStatus;
    if (SUCCEEDED(pThread->GetActiveFrame(&iCorFrame)) && iCorFrame != nullptr &&
        SUCCEEDED(iCorFrame->GetFunction(&iCorFunction)) &&
        SUCCEEDED(iCorFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID*) &iCorFunction2)) &&
        SUCCEEDED(iCorFunction2->GetJMCStatus(&JMCStatus)) &&
        JMCStatus == FALSE)
    {
        return S_OK; // forced to interrupt this callback (breakpoint in not user code, continue process execution)
    }

    if (SUCCEEDED(Status = m_uniqueLineBreakpoints->CheckBreakpointHit(pThread, pBreakpoint, breakpoint)) &&
        Status == S_OK) // S_FALSE - no breakpoint hit
    {
        return S_FALSE; // S_FALSE - not affect on callback (callback will emit stop event)
    }

    if (SUCCEEDED(Status = m_uniqueFuncBreakpoints->CheckBreakpointHit(pThread, pBreakpoint, breakpoint)) &&
        Status == S_OK) // S_FALSE - no breakpoint hit
    {
        return S_FALSE; // S_FALSE - not affect on callback (callback will emit stop event)
    }

    return S_OK; // no breakpoints hit, forced to interrupt this callback
}

HRESULT Breakpoints::ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events)
{
    m_uniqueEntryBreakpoint->ManagedCallbackLoadModule(pModule);
    m_uniqueFuncBreakpoints->ManagedCallbackLoadModule(pModule, events);
    m_uniqueLineBreakpoints->ManagedCallbackLoadModule(pModule, events);
    return S_OK;
}

HRESULT Breakpoints::ManagedCallbackLoadModuleAll(ICorDebugModule *pModule)
{
    m_uniqueHotReloadBreakpoint->ManagedCallbackLoadModuleAll(pModule);
    return S_OK;
}

HRESULT Breakpoints::ManagedCallbackException(ICorDebugThread *pThread, ExceptionCallbackType eventType, const std::string &excModule, StoppedEvent &event)
{
    return m_uniqueExceptionBreakpoints->ManagedCallbackException(pThread, eventType, excModule, event);
}

HRESULT Breakpoints::AllBreakpointsActivate(bool act)
{
    HRESULT Status1 = m_uniqueLineBreakpoints->AllBreakpointsActivate(act);
    HRESULT Status2 = m_uniqueFuncBreakpoints->AllBreakpointsActivate(act);

    return FAILED(Status1) ? Status1 : Status2;
}

HRESULT Breakpoints::BreakpointActivate(uint32_t id, bool act)
{
    if (SUCCEEDED(m_uniqueLineBreakpoints->BreakpointActivate(id, act)))
        return S_OK;

    return m_uniqueFuncBreakpoints->BreakpointActivate(id, act);
}

// This function allows to enumerate breakpoints (sorted by number).
// Callback which is called for each breakpoint might return `false` to stop iteration over breakpoints list.
void Breakpoints::EnumerateBreakpoints(std::function<bool (const IDebugger::BreakpointInfo&)>&& callback)
{
    std::vector<IDebugger::BreakpointInfo> list;
    m_uniqueLineBreakpoints->AddAllBreakpointsInfo(list);
    m_uniqueFuncBreakpoints->AddAllBreakpointsInfo(list);
    m_uniqueExceptionBreakpoints->AddAllBreakpointsInfo(list);
#ifdef INTEROP_DEBUGGING
    m_sharedInteropLineBreakpoints->AddAllBreakpointsInfo(list);
#endif // INTEROP_DEBUGGING

    // sort breakpoint list by ascending order, preserve order of elements with same number
    std::stable_sort(list.begin(), list.end());

    // remove duplicates (ones from m_lineBreakpointMapping which have resolved pair in m_lineResolvedBreakpoints)
    list.erase(std::unique(list.begin(), list.end()), list.end());

    for (const auto &item : list)
    {
        if (!callback(item))
            break;
    }

}

HRESULT Breakpoints::ManagedCallbackExitThread(ICorDebugThread *pThread)
{
    return m_uniqueExceptionBreakpoints->ManagedCallbackExitThread(pThread);
}

HRESULT Breakpoints::CheckApplicationReload(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    return m_uniqueHotReloadBreakpoint->CheckApplicationReload(pThread, pBreakpoint);
}

void Breakpoints::CheckApplicationReload(ICorDebugThread *pThread)
{
    m_uniqueHotReloadBreakpoint->CheckApplicationReload(pThread);
}

HRESULT Breakpoints::SetHotReloadBreakpoint(const std::string &updatedDLL, const std::unordered_set<mdTypeDef> &updatedTypeTokens)
{
    return m_uniqueHotReloadBreakpoint->SetHotReloadBreakpoint(updatedDLL, updatedTypeTokens);
}

#ifdef INTEROP_DEBUGGING

HRESULT Breakpoints::InteropSetLineBreakpoints(pid_t pid, InteropDebugging::InteropLibraries *pInteropLibraries, const std::string& filename,
                                               const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints,
                                               std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    // NOTE interop code provide 'true' on success, we must convert it into HRESULT
    return m_sharedInteropLineBreakpoints->SetLineBreakpoints(pid, pInteropLibraries, filename, lineBreakpoints, breakpoints,
                                                              StopAllThreads, FixAllThreads, [&]() -> uint32_t
    {
        std::lock_guard<std::mutex> lock(m_nextBreakpointIdMutex);
        return m_nextBreakpointId++;
    }) ? S_OK : E_FAIL;
}

bool Breakpoints::InteropSetupRendezvousBrk(pid_t pid, LoadLibCallback loadLibCB, UnloadLibCallback unloadLibCB, IsThumbCodeCallback isThumbCode, int &err_code)
{
    return m_uniqueInteropRendezvousBreakpoint->SetupRendezvousBrk(pid, loadLibCB, unloadLibCB, isThumbCode, err_code);
}

bool Breakpoints::IsInteropBreakpoint(std::uintptr_t brkAddr)
{
    // Make sure we ignore all "internal" breakpoints here.
    if (m_uniqueInteropRendezvousBreakpoint->IsRendezvousBreakpoint(brkAddr))
        return false;

    return m_sharedInteropBreakpoints->IsBreakpoint(brkAddr);
}

bool Breakpoints::IsInteropRendezvousBreakpoint(std::uintptr_t brkAddr)
{
    return m_uniqueInteropRendezvousBreakpoint->IsRendezvousBreakpoint(brkAddr);
}

void Breakpoints::InteropChangeRendezvousState(pid_t TGID, pid_t pid)
{
    m_uniqueInteropRendezvousBreakpoint->ChangeRendezvousState(TGID, pid);
}

bool Breakpoints::IsInteropLineBreakpoint(std::uintptr_t brkAddr, Breakpoint &breakpoint)
{
    return m_sharedInteropLineBreakpoints->IsLineBreakpoint(brkAddr, breakpoint);
}

bool Breakpoints::InteropStepPrevToBrk(pid_t pid, std::uintptr_t brkAddr)
{
    return m_sharedInteropBreakpoints->StepPrevToBrk(pid, brkAddr);
}

void Breakpoints::InteropStepOverBrk(pid_t pid, std::uintptr_t brkAddr)
{
    m_sharedInteropBreakpoints->StepOverBrk(pid, brkAddr);
}

// Must be called only in case all threads stopped and fixed (see InteropDebugger::StopAndDetach()).
void Breakpoints::InteropRemoveAllAtDetach(pid_t pid)
{
    m_sharedInteropBreakpoints->RemoveAllAtDetach(pid);
    m_uniqueInteropRendezvousBreakpoint->RemoveAtDetach(pid);
    m_sharedInteropLineBreakpoints->RemoveAllAtDetach(pid);
}

void Breakpoints::InteropLoadModule(pid_t pid, std::uintptr_t startAddr, InteropDebugging::InteropLibraries *pInteropLibraries, std::vector<BreakpointEvent> &events)
{
    m_sharedInteropLineBreakpoints->LoadModule(pid, startAddr, pInteropLibraries, events);
}

void Breakpoints::InteropUnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr, std::vector<BreakpointEvent> &events)
{
    m_sharedInteropBreakpoints->UnloadModule(startAddr, endAddr);
    m_sharedInteropLineBreakpoints->UnloadModule(startAddr, endAddr, events);
}

HRESULT Breakpoints::InteropAllBreakpointsActivate(pid_t pid, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    // NOTE interop code provide error as `errno` code, we must convert it into HRESULT
    return m_sharedInteropLineBreakpoints->AllBreakpointsActivate(pid, act, StopAllThreads, FixAllThreads) == 0 ? S_OK : E_FAIL;
}

HRESULT Breakpoints::InteropBreakpointActivate(pid_t pid, uint32_t id, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads)
{
    // NOTE interop code provide error as `errno` code, we must convert it into HRESULT
    return m_sharedInteropLineBreakpoints->BreakpointActivate(pid, id, act, StopAllThreads, FixAllThreads) == 0 ? S_OK : E_FAIL;
}

#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
