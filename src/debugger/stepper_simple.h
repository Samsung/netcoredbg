// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include "interfaces/idebugger.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;

class SimpleStepper
{
public:

    SimpleStepper(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules),
        m_justMyCode(true),
        m_enabledSimpleStepId(0)
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
    HRESULT ManagedCallbackStepComplete();

    HRESULT DisableAllSteppers(ICorDebugProcess *pProcess);

    void SetJustMyCode(bool enable) { m_justMyCode = enable; }

private:

    std::shared_ptr<Modules> m_sharedModules;
    bool m_justMyCode;

    std::mutex m_stepMutex;
    int m_enabledSimpleStepId;
};

} // namespace netcoredbg
