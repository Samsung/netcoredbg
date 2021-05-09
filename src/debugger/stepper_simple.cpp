// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/stepper_simple.h"
#include "debugger/threads.h"
#include "interfaces/idebugger.h"
#include "metadata/modules.h"

namespace netcoredbg
{

HRESULT SimpleStepper::SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    CorDebugUnmappedStop stopMask = STOP_NONE;
    IfFailRet(pStepper->SetUnmappedStopMask(stopMask));

    ToRelease<ICorDebugStepper2> pStepper2;
    IfFailRet(pStepper->QueryInterface(IID_ICorDebugStepper2, (LPVOID *)&pStepper2));

    IfFailRet(pStepper2->SetJMC(m_justMyCode));

    ThreadId threadId(getThreadId(pThread));

    if (stepType == IDebugger::STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());

        std::lock_guard<std::mutex> lock(m_stepMutex);
        m_enabledSimpleStepId = int(threadId);

        return S_OK;
    }

    BOOL bStepIn = stepType == IDebugger::STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(m_sharedModules->GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    std::lock_guard<std::mutex> lock(m_stepMutex);
    m_enabledSimpleStepId = int(threadId);

    return S_OK;
}

HRESULT SimpleStepper::ManagedCallbackBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    ThreadId threadId(getThreadId(pThread));

    auto stepForcedIgnoreBP = [&] ()
    {
        {
            std::lock_guard<std::mutex> lock(m_stepMutex);
            if (m_enabledSimpleStepId != int(threadId))
            {
                return false;
            }
        }

        ToRelease<ICorDebugStepperEnum> steppers;
        if (FAILED(pAppDomain->EnumerateSteppers(&steppers)))
            return false;

        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            BOOL pbActive;
            ToRelease<ICorDebugStepper> pStepper(curStepper);
            if (SUCCEEDED(pStepper->IsActive(&pbActive)) && pbActive)
                return false;
        }

        return true;
    };

    if (stepForcedIgnoreBP())
    {
        pAppDomain->Continue(0);
        return S_OK;  
    }

    return S_FALSE; // S_FAIL - no error, but steppers not affect on callback
}

HRESULT SimpleStepper::ManagedCallbackStepComplete()
{
    // Reset simple step without real stepper release.
    m_stepMutex.lock();
    m_enabledSimpleStepId = 0;
    m_stepMutex.unlock();

    return S_FALSE; // S_FAIL - no error, but steppers not affect on callback
}

HRESULT SimpleStepper::DisableAllSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        ToRelease<ICorDebugStepperEnum> steppers;
        IfFailRet(pDomain->EnumerateSteppers(&steppers));

        ICorDebugStepper *curStepper;
        ULONG steppersFetched;
        while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
        {
            ToRelease<ICorDebugStepper> pStepper(curStepper);
            pStepper->Deactivate();
        }
    }

    m_stepMutex.lock();
    m_enabledSimpleStepId = 0;
    m_stepMutex.unlock();

    return S_OK;
}

} // namespace netcoredbg
