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
class Evaluator;

class Steppers
{
public:

    Steppers(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Evaluator> &sharedEvaluator) :
        m_simpleStepper(new SimpleStepper(sharedModules)),
        m_asyncStepper(new AsyncStepper(m_simpleStepper, sharedModules, sharedEvaluator)),
        m_sharedModules(sharedModules),
        m_justMyCode(true)
    {}

    HRESULT SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pProcess->Continue(0);
    // Good:
    //     IfFailRet(pProcess->Continue(0));
    //     return S_OK;
    HRESULT ManagedCallbackBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread);
    HRESULT ManagedCallbackStepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, CorDebugStepReason reason, StackFrame &stackFrame);

    HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);
    HRESULT DisableAllSimpleSteppers(ICorDebugProcess *pProcess);

    void SetJustMyCode(bool enable);

private:

    std::shared_ptr<SimpleStepper> m_simpleStepper;
    std::unique_ptr<AsyncStepper> m_asyncStepper;
    std::shared_ptr<Modules> m_sharedModules;
    bool m_justMyCode;
};

} // namespace netcoredbg
