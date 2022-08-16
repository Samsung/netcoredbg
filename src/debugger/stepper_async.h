// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include "interfaces/idebugger.h"
#include "utils/torelease.h"
#include "metadata/async_info.h"


namespace netcoredbg
{

class AsyncInfo;
class EvalHelpers;
class SimpleStepper;

class AsyncStepper
{
public:

    AsyncStepper(std::shared_ptr<SimpleStepper> simpleStepper, std::shared_ptr<Modules> &sharedModules, std::shared_ptr<EvalHelpers> &sharedEvalHelpers) :
        m_simpleStepper(simpleStepper),
        m_uniqueAsyncInfo(new AsyncInfo(sharedModules)),
        m_sharedEvalHelpers(sharedEvalHelpers),
        m_asyncStep(nullptr),
        m_asyncStepNotifyDebuggerOfWaitCompletion(nullptr)
    {}

    HRESULT SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pThread->GetID(&threadId);
    // Good:
    //     IfFailRet(pThread->GetID(&threadId));
    //     return S_OK;
    HRESULT ManagedCallbackBreakpoint(ICorDebugThread *pThread);
    HRESULT ManagedCallbackStepComplete();

    HRESULT DisableAllSteppers();

private:

    std::shared_ptr<SimpleStepper> m_simpleStepper;
    std::unique_ptr<AsyncInfo> m_uniqueAsyncInfo;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;

    enum class asyncStepStatus
    {
        yield_offset_breakpoint,
        resume_offset_breakpoint
    };

    struct asyncBreakpoint_t
    {
        ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 ilOffset;

        asyncBreakpoint_t() :
            iCorFuncBreakpoint(nullptr),
            modAddress(0),
            methodToken(0),
            ilOffset(0)
        {}

        ~asyncBreakpoint_t()
        {
            if (iCorFuncBreakpoint)
                iCorFuncBreakpoint->Activate(FALSE);
        }
    };

    struct asyncStep_t
    {
        ThreadId m_threadId;
        IDebugger::StepType m_initialStepType;
        uint32_t m_resume_offset;
        asyncStepStatus m_stepStatus;
        std::unique_ptr<asyncBreakpoint_t> m_Breakpoint;
        ToRelease<ICorDebugHandleValue> m_iCorHandleValueAsyncId;

        asyncStep_t() :
            m_threadId(ThreadId::Invalid),
            m_initialStepType(IDebugger::StepType::STEP_OVER),
            m_resume_offset(0),
            m_stepStatus(asyncStepStatus::yield_offset_breakpoint),
            m_Breakpoint(nullptr),
            m_iCorHandleValueAsyncId(nullptr)
        {}
    };

    std::mutex m_asyncStepMutex;
    // Pointer to object, that provide all active async step related data. Object will be created only in case of active async method stepping.
    std::unique_ptr<asyncStep_t> m_asyncStep;
    // System.Threading.Tasks.Task.NotifyDebuggerOfWaitCompletion() method function breakpoint data, will be configured at async method step-out setup.
    std::unique_ptr<asyncBreakpoint_t> m_asyncStepNotifyDebuggerOfWaitCompletion;

    HRESULT SetBreakpointIntoNotifyDebuggerOfWaitCompletion();
};

} // namespace netcoredbg
