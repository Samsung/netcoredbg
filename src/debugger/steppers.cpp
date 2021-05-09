// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "metadata/modules.h"

namespace netcoredbg
{

HRESULT Steppers::SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
    DisableAllSteppers(pProcess);

    IfFailRet(m_asyncStepper->SetupStep(pThread, stepType));
    if (Status == S_OK) // S_FALSE = setup simple stepper
        return S_OK;

    return m_simpleStepper->SetupStep(pThread, stepType);
}

HRESULT Steppers::ManagedCallbackBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    HRESULT Status;
    // Check async stepping related breakpoints first, since user can't setup breakpoints to await block yield or resume offsets manually,
    // so, async stepping related breakpoints not a part of any user breakpoints related data (that will be checked in separate thread. see code below).
    IfFailRet(m_asyncStepper->ManagedCallbackBreakpoint(pAppDomain, pThread));
    if (Status == S_OK) // S_FAIL - no error, but steppers not affect on callback
        return S_OK;

    return m_simpleStepper->ManagedCallbackBreakpoint(pAppDomain, pThread);
}

HRESULT Steppers::ManagedCallbackStepComplete(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, CorDebugStepReason reason, StackFrame &stackFrame)
{
    m_simpleStepper->ManagedCallbackStepComplete();
    m_asyncStepper->ManagedCallbackStepComplete();

    // No reason check GetFrameLocation() return code, since it could be failed by some API call after source detection.
    const bool no_source = stackFrame.source.IsNull();

    HRESULT Status;
    if (m_justMyCode && no_source)
    {
        // FIXME will not work in proper way with async methods
        IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OVER));
        pAppDomain->Continue(0);
        return S_OK;
    }

    // Note, step-in stop at the beginning of a newly called function at 0 IL offset.
    // But, for example, async function don't have user code at 0 IL offset, so,
    // we need additional step-over to real user code.
    if (reason == CorDebugStepReason::STEP_CALL)
    {
        ToRelease<ICorDebugFrame> pFrame;
        ToRelease<ICorDebugILFrame> pILFrame;
        ULONG32 ipOffset;
        CorDebugMappingResult mappingResult;
        Modules::SequencePoint sequencePoint;
        if (SUCCEEDED(pThread->GetActiveFrame(&pFrame)) && pFrame != nullptr &&
            SUCCEEDED(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame)) &&
            SUCCEEDED(pILFrame->GetIP(&ipOffset, &mappingResult)) &&
            SUCCEEDED(m_sharedModules->GetFrameILAndSequencePoint(pFrame, ipOffset, sequencePoint)) &&
            // Current IL offset less than real offset of first user code line.
            ipOffset < (ULONG32)sequencePoint.offset)
        {
            IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OVER));
            pAppDomain->Continue(0);
            return S_OK;
        }
    }

    return S_FALSE; // S_FAIL - no error, but steppers not affect on callback
}

HRESULT Steppers::DisableAllSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;
    IfFailRet(m_simpleStepper->DisableAllSteppers(pProcess));
    return m_asyncStepper->DisableAllSteppers();
}

HRESULT Steppers::DisableAllSimpleSteppers(ICorDebugProcess *pProcess)
{
    return m_simpleStepper->DisableAllSteppers(pProcess);
}

void Steppers::SetJustMyCode(bool enable)
{
    m_justMyCode = enable;
    m_simpleStepper->SetJustMyCode(enable);
}

} // namespace netcoredbg
