// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <memory>
#include "interfaces/idebugger.h"

namespace netcoredbg
{

class SimpleStepper;
class AsyncStepper;
class Modules;
class EvalHelpers;

class Steppers
{
public:

    Steppers(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<EvalHelpers> &sharedEvalHelpers) :
        m_simpleStepper(new SimpleStepper(sharedModules)),
        m_asyncStepper(new AsyncStepper(m_simpleStepper, sharedModules, sharedEvalHelpers)),
        m_sharedModules(sharedModules),
        m_initialStepType(IDebugger::StepType::STEP_OVER),
        m_justMyCode(true),
        m_stepFiltering(true),
        m_filteredPrevStep(false)
    {}

    HRESULT SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pThread->GetID(&threadId);
    // Good:
    //     IfFailRet(pThread->GetID(&threadId));
    //     return S_OK;
    HRESULT ManagedCallbackBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread);
    HRESULT ManagedCallbackStepComplete(ICorDebugThread *pThread, CorDebugStepReason reason);

    HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);
    HRESULT DisableAllSteppers(ICorDebugAppDomain *pAppDomain);
    HRESULT DisableAllSimpleSteppers(ICorDebugProcess *pProcess);

    void SetJustMyCode(bool enable);
    void SetStepFiltering(bool enable);

private:

    std::shared_ptr<SimpleStepper> m_simpleStepper;
    std::unique_ptr<AsyncStepper> m_asyncStepper;
    std::shared_ptr<Modules> m_sharedModules;
    IDebugger::StepType m_initialStepType;
    Modules::SequencePoint m_StepStartSP;
    bool m_justMyCode;
    // https://docs.microsoft.com/en-us/visualstudio/debugger/navigating-through-code-with-the-debugger?view=vs-2019#BKMK_Step_into_properties_and_operators_in_managed_code
    // The debugger steps over properties and operators in managed code by default. In most cases, this provides a better debugging experience.
    bool m_stepFiltering;
    // Previous step-in was made in method that must not be stepped. We need store this information in order to step-in again as soon, as we leave this method.
    // Usually this is code related to m_stepFiltering, but in some cases we could also filter compiler generated code and code covered by StepThrough attribute.
    bool m_filteredPrevStep;
};

} // namespace netcoredbg
