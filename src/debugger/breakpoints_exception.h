// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include "interfaces/types.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <mutex>
#include <set>

namespace netcoredbg
{

class Evaluator;
class Variables;

class ExceptionBreakpoints
{
public:

    ExceptionBreakpoints(std::shared_ptr<Variables> &sharedVariables, std::shared_ptr<Evaluator> &sharedEvaluator) :
        m_sharedVariables(sharedVariables),
        m_sharedEvaluator(sharedEvaluator)
    {}

    // TODO DeleteAll()

    HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t id);
    HRESULT DeleteExceptionBreakpoint(uint32_t id);
    HRESULT GetExceptionInfoResponse(ICorDebugProcess *pProcess, ThreadId threadId, ExceptionInfoResponse &exceptionInfoResponse);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pThread->GetID(&threadId);
    // Good:
    //     IfFailRet(pThread->GetID(&threadId));
    //     return S_OK;
    HRESULT ManagedCallbackException(ICorDebugThread *pThread, CorDebugExceptionCallbackType dwEventType, StoppedEvent &event, std::string &textOutput);

private:

    HRESULT GetExceptionBreakMode(ExceptionBreakMode &mode, const std::string &name);
    bool MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category);

    std::shared_ptr<Variables> m_sharedVariables;
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    
    std::mutex m_lastUnhandledExceptionThreadIdsMutex;
    std::set<ThreadId> m_lastUnhandledExceptionThreadIds;

    struct ExceptionBreakpointStorage
    {
    private:
        // vsdbg not supported list of exception breakpoint command
        struct ExceptionBreakpoint {
            ExceptionBreakpoint() : current_asterix_id(0) {}
            std::unordered_map<uint32_t, std::string> table;
            // For global filter (*) we need to know last id
            uint32_t current_asterix_id;
            // for customers its will to come some difficult for matching.
            // For netcoredbg approach based on single unique name for each
            // next of user exception.
            //std::unordered_map<std::string, ExceptionBreakMode> exceptionBreakpoints;
            std::unordered_multimap<std::string, ExceptionBreakMode> exceptionBreakpoints;
        };

        ExceptionBreakpoint bp;

    public:
        HRESULT Insert(uint32_t id, const ExceptionBreakMode &mode, const std::string &name);
        HRESULT Delete(uint32_t id);
        bool Match(int dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category) const;
        HRESULT GetExceptionBreakMode(ExceptionBreakMode &out, const std::string &name) const;

        ExceptionBreakpointStorage() = default;
        ExceptionBreakpointStorage(ExceptionBreakpointStorage &&that) = default;
        ExceptionBreakpointStorage(const ExceptionBreakpointStorage &that) = delete;
    };

    std::mutex m_exceptionBreakpointsMutex;
    ExceptionBreakpointStorage m_exceptionBreakpoints;
};

} // namespace netcoredbg
