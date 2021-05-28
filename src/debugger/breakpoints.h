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

    Breakpoints(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Variables> &sharedVariables, std::shared_ptr<Evaluator> &sharedEvaluator) :
        m_uniqueBreakBreakpoint(new BreakBreakpoint(sharedModules)),
        m_uniqueEntryBreakpoint(new EntryBreakpoint(sharedModules)),
        m_uniqueExceptionBreakpoints(new ExceptionBreakpoints(sharedVariables, sharedEvaluator)),
        m_uniqueFuncBreakpoints(new FuncBreakpoints(sharedModules)),
        m_uniqueLineBreakpoints(new LineBreakpoints(sharedModules)),
        m_nextBreakpointId(1)
    {}

    void SetJustMyCode(bool enable);
    void SetLastStoppedIlOffset(ICorDebugProcess *pProcess, const ThreadId &lastStoppedThreadId);
    void SetStopAtEntry(bool enable);
    void DeleteAll();
    HRESULT DisableAll(ICorDebugProcess *pProcess);

    HRESULT SetFuncBreakpoints(ICorDebugProcess *pProcess, const std::vector<FuncBreakpoint> &funcBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT SetLineBreakpoints(ICorDebugProcess *pProcess, const std::string &filename,
                               const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints);

    HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t &id);
    HRESULT DeleteExceptionBreakpoint(const uint32_t id);
    HRESULT GetExceptionInfoResponse(ICorDebugProcess *pProcess, ThreadId threadId, ExceptionInfoResponse &exceptionInfoResponse);

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
    HRESULT ManagedCallbackBreakpoint(IDebugger *debugger, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint, bool &atEntry);
    HRESULT ManagedCallbackException(ICorDebugThread *pThread, CorDebugExceptionCallbackType dwEventType, StoppedEvent &event, std::string &textOutput);
    HRESULT ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

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
