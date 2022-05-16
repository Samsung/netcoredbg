// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>
#include <mutex>
#include <memory>
#include "interfaces/idebugger.h"

namespace netcoredbg
{

class Modules;
class BreakBreakpoint;
class EntryBreakpoint;
class ExceptionBreakpoints;
class FuncBreakpoints;
class LineBreakpoints;

class Breakpoints
{
public:

    Breakpoints(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Evaluator> &sharedEvaluator, std::shared_ptr<Variables> &sharedVariables) :
        m_uniqueBreakBreakpoint(new BreakBreakpoint(sharedModules)),
        m_uniqueEntryBreakpoint(new EntryBreakpoint(sharedModules)),
        m_uniqueExceptionBreakpoints(new ExceptionBreakpoints(sharedEvaluator)),
        m_uniqueFuncBreakpoints(new FuncBreakpoints(sharedModules, sharedVariables)),
        m_uniqueLineBreakpoints(new LineBreakpoints(sharedModules, sharedVariables)),
        m_nextBreakpointId(1)
    {}

    void SetJustMyCode(bool enable);
    void SetLastStoppedIlOffset(ICorDebugProcess *pProcess, const ThreadId &lastStoppedThreadId);
    void SetStopAtEntry(bool enable);
    void DeleteAll();
    HRESULT DisableAll(ICorDebugProcess *pProcess);

    HRESULT SetFuncBreakpoints(bool haveProcess, const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetLineBreakpoints(bool haveProcess, const std::string &filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints);
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
    HRESULT ManagedCallbackExitThread(ICorDebugThread *pThread);

private:

    std::unique_ptr<BreakBreakpoint> m_uniqueBreakBreakpoint;
    std::unique_ptr<EntryBreakpoint> m_uniqueEntryBreakpoint;
    std::unique_ptr<ExceptionBreakpoints> m_uniqueExceptionBreakpoints;
    std::unique_ptr<FuncBreakpoints> m_uniqueFuncBreakpoints;
    std::unique_ptr<LineBreakpoints> m_uniqueLineBreakpoints;

    std::mutex m_nextBreakpointIdMutex;
    uint32_t m_nextBreakpointId;

};

} // namespace netcoredbg
