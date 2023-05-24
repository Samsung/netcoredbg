// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>
#include <mutex>
#include <memory>
#include <unordered_set>
#include "interfaces/idebugger.h"
#include "debugger/interop_ptrace_helpers.h"

namespace netcoredbg
{

class Evaluator;
class EvalHelpers;
class Variables;
class Modules;
class BreakBreakpoint;
class EntryBreakpoint;
class ExceptionBreakpoints;
class FuncBreakpoints;
class LineBreakpoints;
class HotReloadBreakpoint;
#ifdef INTEROP_DEBUGGING
namespace InteropDebugging
{
class InteropRendezvousBreakpoint;
class InteropBreakpoints;
class InteropLineBreakpoints;
class InteropLibraries;
} // namespace InteropDebugging
#endif // INTEROP_DEBUGGING

class Breakpoints
{
public:

    Breakpoints(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Evaluator> &sharedEvaluator, std::shared_ptr<EvalHelpers> &sharedEvalHelpers, std::shared_ptr<Variables> &sharedVariables);

    void SetJustMyCode(bool enable);
    void SetLastStoppedIlOffset(ICorDebugProcess *pProcess, const ThreadId &lastStoppedThreadId);
    void SetStopAtEntry(bool enable);
    void DeleteAllManaged();
    HRESULT DisableAllManaged(ICorDebugProcess *pProcess);

    HRESULT UpdateLineBreakpoint(bool haveProcess, int id, int linenum, Breakpoint &breakpoint);
    HRESULT SetFuncBreakpoints(bool haveProcess, const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetLineBreakpoints(bool haveProcess, const std::string &filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetHotReloadBreakpoint(const std::string &updatedDLL, const std::unordered_set<mdTypeDef> &updatedTypeTokens);
    HRESULT UpdateBreakpointsOnHotReload(ICorDebugModule *pModule, std::unordered_set<mdMethodDef> &methodTokens, std::vector<BreakpointEvent> &events);

    HRESULT GetExceptionInfo(ICorDebugThread *pThread, ExceptionInfo &exceptionInfo);

    void EnumerateBreakpoints(std::function<bool (const IDebugger::BreakpointInfo&)>&& callback);
    HRESULT BreakpointActivate(uint32_t id, bool act);
    HRESULT AllBreakpointsActivate(bool act);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pThread->GetID(&threadId);
    // Good:
    //     IfFailRet(pThread->GetID(&threadId));
    //     return S_OK;
    HRESULT ManagedCallbackBreak(ICorDebugThread *pThread, const ThreadId &lastStoppedThreadId);
    HRESULT ManagedCallbackBreakpoint(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint, bool &atEntry);
    HRESULT ManagedCallbackException(ICorDebugThread *pThread, ExceptionCallbackType eventType, const std::string &excModule, StoppedEvent &event);
    HRESULT ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);
    HRESULT ManagedCallbackLoadModuleAll(ICorDebugModule *pModule);
    HRESULT ManagedCallbackExitThread(ICorDebugThread *pThread);

    // S_OK - internal HotReload breakpoint hit
    // S_FALSE - not internal HotReload breakpoint hit
    HRESULT CheckApplicationReload(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);
    void CheckApplicationReload(ICorDebugThread *pThread);

#ifdef INTEROP_DEBUGGING
    HRESULT InteropSetLineBreakpoints(pid_t pid, InteropDebugging::InteropLibraries *pInteropLibraries, const std::string& filename,
                                      const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints,
                                      std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    HRESULT InteropAllBreakpointsActivate(pid_t pid, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    HRESULT InteropBreakpointActivate(pid_t pid, uint32_t id, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    // In case of error - return `false`.
    typedef std::function<void(pid_t, const std::string&, const std::string&, std::uintptr_t, std::uintptr_t)> LoadLibCallback;
    typedef std::function<void(const std::string&)> UnloadLibCallback;
    typedef std::function<bool(std::uintptr_t)> IsThumbCodeCallback;
    bool InteropSetupRendezvousBrk(pid_t pid, LoadLibCallback loadLibCB, UnloadLibCallback unloadLibCB, IsThumbCodeCallback isThumbCode, int &err_code);
    // Return true, if execution stop at user's native breakpoint (fast check).
    bool IsInteropBreakpoint(std::uintptr_t brkAddr);
    bool IsInteropRendezvousBreakpoint(std::uintptr_t brkAddr);
    void InteropChangeRendezvousState(pid_t TGID, pid_t pid);
    bool IsInteropLineBreakpoint(std::uintptr_t brkAddr, Breakpoint &breakpoint);
    // In case we stop at breakpoint and need just move PC before breakpoint.
    // Note, this method will reset PC in case thread stop at breakpoint and alter `regs`.
    bool InteropStepPrevToBrk(pid_t pid, std::uintptr_t brkAddr);
    // Execute real breakpoint's code with single step.
    void InteropStepOverBrk(pid_t pid, std::uintptr_t brkAddr, std::function<bool(pid_t, std::uintptr_t)> SingleStepOnBrk);
    // Remove all native breakpoints at interop detach.
    void InteropRemoveAllAtDetach(pid_t pid);
    // Resolve breakpoints for module.
    void InteropLoadModule(pid_t pid, std::uintptr_t startAddr, InteropDebugging::InteropLibraries *pInteropLibraries, std::vector<BreakpointEvent> &events);
    // Remove all related to unloaded library breakpoints entries in data structures.
    void InteropUnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr, std::vector<BreakpointEvent> &events);
#endif // INTEROP_DEBUGGING

private:

    std::unique_ptr<BreakBreakpoint> m_uniqueBreakBreakpoint;
    std::unique_ptr<EntryBreakpoint> m_uniqueEntryBreakpoint;
    std::unique_ptr<ExceptionBreakpoints> m_uniqueExceptionBreakpoints;
    std::unique_ptr<FuncBreakpoints> m_uniqueFuncBreakpoints;
    std::unique_ptr<LineBreakpoints> m_uniqueLineBreakpoints;
    std::unique_ptr<HotReloadBreakpoint> m_uniqueHotReloadBreakpoint;
#ifdef INTEROP_DEBUGGING
    // "Low level" native breakpoints layer (related to memory patch).
    std::shared_ptr<InteropDebugging::InteropBreakpoints> m_sharedInteropBreakpoints;
    // "Up level" rendezvous breakpoint connected to libs load/unload routine.
    std::unique_ptr<InteropDebugging::InteropRendezvousBreakpoint> m_uniqueInteropRendezvousBreakpoint;
    // "Up level" line breakpoints implementation.
    // m_sharedInteropBreakpoints is "low level" layer for work with memory directly. m_sharedInteropLineBreakpoints is top of m_sharedInteropBreakpoints
    // implementation for line breakpoint logic (close to managed line breakpoints we already have). In case of managed line breakpoints,
    // "low level" layer is CoreCLR debug API itself.
    std::unique_ptr<InteropDebugging::InteropLineBreakpoints> m_sharedInteropLineBreakpoints;
#endif // INTEROP_DEBUGGING

    std::mutex m_nextBreakpointIdMutex;
    uint32_t m_nextBreakpointId;

};

} // namespace netcoredbg
