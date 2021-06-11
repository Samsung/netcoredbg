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
#include <functional>

namespace netcoredbg
{

class Evaluator;

class ExceptionBreakpoints
{
public:

    ExceptionBreakpoints(std::shared_ptr<Evaluator> &sharedEvaluator) :
        m_sharedEvaluator(sharedEvaluator),
        m_justMyCode(true),
        m_exceptionBreakpoints((size_t)ExceptionBreakpointFilter::Size)
    {}

    void SetJustMyCode(bool enable) { m_justMyCode = enable; };
    void DeleteAll();
    HRESULT SetExceptionBreakpoints(const std::vector<ExceptionBreakpoint> &exceptionBreakpoints, std::vector<Breakpoint> &breakpoints,
                                    std::function<uint32_t()> getId);
    HRESULT GetExceptionInfo(ICorDebugThread *pThread, ExceptionInfo &exceptionInfo);
    bool CoveredByFilter(ExceptionBreakpointFilter filterId, const std::string &excType, ExceptionCategory excCategory);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pThread->GetID(&threadId);
    // Good:
    //     IfFailRet(pThread->GetID(&threadId));
    //     return S_OK;
    HRESULT ManagedCallbackException(ICorDebugThread *pThread, ExceptionCallbackType eventType, std::string excModule, StoppedEvent &event);
    HRESULT ManagedCallbackExitThread(ICorDebugThread *pThread);

private:

    std::shared_ptr<Evaluator> m_sharedEvaluator;
    bool m_justMyCode;

    struct ExceptionStatus
    {
        ExceptionCallbackType m_lastEvent;
        std::string m_excModule;

        ExceptionStatus() :
            m_lastEvent(ExceptionCallbackType::FIRST_CHANCE)
        {}
    };

    std::mutex m_threadsExceptionMutex;
    std::unordered_map<DWORD, ExceptionStatus> m_threadsExceptionStatus;
    // Note, we have Exception callback called with different exception callback type, and we need know exception type that related to current stop event.
    std::unordered_map<DWORD, ExceptionBreakMode> m_threadsExceptionBreakMode;

    HRESULT GetExceptionDetails(ICorDebugThread *pThread, ICorDebugValue *pExceptionValue, ExceptionDetails &details);

    struct ManagedExceptionBreakpoint
    {
        uint32_t id;
        ExceptionCategory categoryHint;
        std::unordered_set<std::string> condition; // Note, only exception type related conditions allowed for now.
        bool negativeCondition;

        ManagedExceptionBreakpoint() :
            id(0), categoryHint(ExceptionCategory::ANY), negativeCondition(false)
        {}

        void ToBreakpoint(Breakpoint &breakpoint) const;

        ManagedExceptionBreakpoint(ManagedExceptionBreakpoint &&that) = default;
        ManagedExceptionBreakpoint(const ManagedExceptionBreakpoint &that) = delete;
    };

    std::mutex m_breakpointsMutex;
    std::vector<std::unordered_multimap<std::string, ManagedExceptionBreakpoint>> m_exceptionBreakpoints;

};

} // namespace netcoredbg
