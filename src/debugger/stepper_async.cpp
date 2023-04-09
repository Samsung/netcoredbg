// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/threads.h"
#include "metadata/typeprinter.h"
#include "debugger/evalhelpers.h"
#include "debugger/valueprint.h"
#include "utils/utf.h"

namespace netcoredbg
{

// Get '<>t__builder' field value for builder from frame.
// [in] pFrame - frame that used for get all info needed (function, module, etc);
// [out] ppValue_builder - result value.
static HRESULT GetAsyncTBuilder(ICorDebugFrame *pFrame, ICorDebugValue **ppValue_builder)
{
    HRESULT Status;

    // Find 'this'.
    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));
    ToRelease<ICorDebugModule> pModule_this;
    IfFailRet(pFunction->GetModule(&pModule_this));
    ToRelease<IUnknown> pMDUnknown_this;
    IfFailRet(pModule_this->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown_this));
    ToRelease<IMetaDataImport> pMD_this;
    IfFailRet(pMDUnknown_this->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD_this));
    mdMethodDef methodDef;
    IfFailRet(pFunction->GetToken(&methodDef));
    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
    ToRelease<ICorDebugValueEnum> pParamEnum;
    IfFailRet(pILFrame->EnumerateArguments(&pParamEnum));
    ULONG cParams = 0;
    IfFailRet(pParamEnum->GetCount(&cParams));
    if (cParams == 0)
        return E_FAIL;
    DWORD methodAttr = 0;
    IfFailRet(pMD_this->GetMethodProps(methodDef, NULL, NULL, 0, NULL, &methodAttr, NULL, NULL, NULL, NULL));
    bool thisParam = (methodAttr & mdStatic) == 0;
    if (!thisParam)
        return E_FAIL;
    // At this point, first param will be always 'this'.
    ToRelease<ICorDebugValue> pRefValue_this;
    IfFailRet(pParamEnum->Next(1, &pRefValue_this, nullptr));


    // Find '<>t__builder' field.
    ToRelease<ICorDebugValue> pValue_this;
    IfFailRet(DereferenceAndUnboxValue(pRefValue_this, &pValue_this, nullptr));
    ToRelease<ICorDebugValue2> pValue2_this;
    IfFailRet(pValue_this->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2_this));
    ToRelease<ICorDebugType> pType_this;
    IfFailRet(pValue2_this->GetExactType(&pType_this));
    ToRelease<ICorDebugClass> pClass_this;
    IfFailRet(pType_this->GetClass(&pClass_this));
    mdTypeDef typeDef_this;
    IfFailRet(pClass_this->GetToken(&typeDef_this));

    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    ToRelease<ICorDebugValue> pRefValue_t__builder;
    while(SUCCEEDED(pMD_this->EnumFields(&hEnum, typeDef_this, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        WCHAR mdName[mdNameLen] = {0};
        if (FAILED(pMD_this->GetFieldProps(fieldDef, nullptr, mdName, _countof(mdName), &nameLen,
                                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)))
        {
            continue;
        }

        if (!str_equal(mdName, W("<>t__builder")))
            continue;

        ToRelease<ICorDebugObjectValue> pObjValue_this;
        if (SUCCEEDED(pValue_this->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue_this)))
            pObjValue_this->GetFieldValue(pClass_this, fieldDef, &pRefValue_t__builder);

        break;
    }
    pMD_this->CloseEnum(hEnum);

    if (pRefValue_t__builder == nullptr)
        return E_FAIL;
    IfFailRet(DereferenceAndUnboxValue(pRefValue_t__builder, ppValue_builder, nullptr));

    return S_OK;
}

// Find Async ID, in our case - reference to created by builder object,
// that could be use as unique ID for builder (state machine) on yield and resume offset breakpoints.
// [in] pThread - managed thread for evaluation (related to pFrame);
// [in] pFrame - frame that used for get all info needed (function, module, etc);
// [in] pEvalHelpers - pointer to managed debugger EvalHelpers;
// [out] ppValueAsyncIdRef - result value (reference to created by builder object).
static HRESULT GetAsyncIdReference(ICorDebugThread *pThread, ICorDebugFrame *pFrame, EvalHelpers *pEvalHelpers, ICorDebugValue **ppValueAsyncIdRef)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(GetAsyncTBuilder(pFrame, &pValue));

    // Find 'ObjectIdForDebugger' property.
    ToRelease<ICorDebugValue2> pValue2;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    ToRelease<ICorDebugType> pType;
    IfFailRet(pValue2->GetExactType(&pType));
    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    mdMethodDef mdObjectIdForDebuggerGetter = mdMethodDefNil;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        ULONG propertyNameLen = 0;
        WCHAR propertyName[mdNameLen] = W("\0");
        mdMethodDef mdGetter = mdMethodDefNil;
        if (FAILED(pMD->GetPropertyProps(propertyDef, nullptr, propertyName, _countof(propertyName), &propertyNameLen,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &mdGetter, nullptr, 0, nullptr)))
        {
            continue;
        }

        if (!str_equal(propertyName, W("ObjectIdForDebugger")))
            continue;

        mdObjectIdForDebuggerGetter = mdGetter;
        break;
    }
    pMD->CloseEnum(propEnum);

    if (mdObjectIdForDebuggerGetter == mdMethodDefNil)
        return E_FAIL;

    // Call 'ObjectIdForDebugger' property getter.
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(mdObjectIdForDebuggerGetter, &pFunc));
    // Note, builder (`this` value) could be generic type - Task<TResult>, type must be provided too.
    IfFailRet(pEvalHelpers->EvalFunction(pThread, pFunc, pType.GetRef(), 1, pValue.GetRef(), 1, ppValueAsyncIdRef, defaultEvalFlags));

    return S_OK;
}

// Set notification for wait completion - call SetNotificationForWaitCompletion() method for particular builder.
// [in] pThread - managed thread for evaluation (related to pFrame);
// [in] pFrame - frame that used for get all info needed (function, module, etc);
// [in] pEvalHelpers - pointer to managed debugger EvalHelpers;
static HRESULT SetNotificationForWaitCompletion(ICorDebugThread *pThread, ICorDebugFrame *pFrame, ICorDebugValue *pBuilderValue, EvalHelpers *pEvalHelpers)
{
    HRESULT Status;

    // Find SetNotificationForWaitCompletion() method.
    ToRelease<ICorDebugValue2> pValue2;
    IfFailRet(pBuilderValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    ToRelease<ICorDebugType> pType;
    IfFailRet(pValue2->GetExactType(&pType));
    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numMethods = 0;
    HCORENUM hEnum = NULL;
    mdMethodDef methodDef;
    mdMethodDef setNotifDef = mdMethodDefNil;
    while(SUCCEEDED(pMD->EnumMethods(&hEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
    {
        mdTypeDef memTypeDef;
        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};
        if (FAILED(pMD->GetMethodProps(methodDef, &memTypeDef,
                                       szFunctionName, _countof(szFunctionName), &nameLen,
                                       nullptr, nullptr, nullptr, nullptr, nullptr)))
        {
            continue;
        }

        if (!str_equal(szFunctionName, W("SetNotificationForWaitCompletion")))
            continue;

        setNotifDef = methodDef;
        break;
    }
    pMD->CloseEnum(hEnum);

    if (setNotifDef == mdMethodDefNil)
        return E_FAIL;

    // Create boolean argument and set it to TRUE.
    ToRelease<ICorDebugEval> pEval;
    IfFailRet(pThread->CreateEval(&pEval));
    ToRelease<ICorDebugValue> pNewBoolean;
    IfFailRet(pEval->CreateValue(ELEMENT_TYPE_BOOLEAN, nullptr, &pNewBoolean));
    ULONG32 cbSize;
    IfFailRet(pNewBoolean->GetSize(&cbSize));
    std::unique_ptr<BYTE[]> rgbValue(new (std::nothrow) BYTE[cbSize]);
    if (rgbValue == nullptr)
        return E_OUTOFMEMORY;
    memset(rgbValue.get(), 0, cbSize * sizeof(BYTE));
    ToRelease<ICorDebugGenericValue> pGenericValue;
    IfFailRet(pNewBoolean->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
    IfFailRet(pGenericValue->GetValue((LPVOID) &(rgbValue[0])));
    rgbValue[0] = 1; // TRUE
    IfFailRet(pGenericValue->SetValue((LPVOID) &(rgbValue[0])));

    // Call this.<>t__builder.SetNotificationForWaitCompletion(TRUE).
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(setNotifDef, &pFunc));

    ICorDebugValue *ppArgsValue[] = {pBuilderValue, pNewBoolean};
    // Note, builder (`this` value) could be generic type - Task<TResult>, type must be provided too.
    IfFailRet(pEvalHelpers->EvalFunction(pThread, pFunc, pType.GetRef(), 1, ppArgsValue, 2, nullptr, defaultEvalFlags));

    return S_OK;
}

HRESULT AsyncStepper::SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));
    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));
    ULONG32 methodVersion;
    IfFailRet(pCode->GetVersionNumber(&methodVersion));

    if (!m_uniqueAsyncInfo->IsMethodHaveAwait(modAddress, methodToken, methodVersion))
        return S_FALSE; // setup simple stepper

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 ipOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ipOffset, &mappingResult));

    // If we are at end of async method with await blocks and doing step-in or step-over,
    // switch to step-out, so whole NotifyDebuggerOfWaitCompletion magic happens.
    ULONG32 lastIlOffset;
    if (stepType != IDebugger::StepType::STEP_OUT &&
        m_uniqueAsyncInfo->FindLastIlOffsetAwaitInfo(modAddress, methodToken, methodVersion, lastIlOffset) &&
        ipOffset >= lastIlOffset)
    {
        stepType = IDebugger::StepType::STEP_OUT;
    }
    if (stepType == IDebugger::StepType::STEP_OUT)
    {
        ToRelease<ICorDebugValue> pBuilderValue;
        IfFailRet(GetAsyncTBuilder(pFrame, &pBuilderValue));

        // In case method is "async void", builder is "System.Runtime.CompilerServices.AsyncVoidMethodBuilder"
        // "If we are inside `async void` method, do normal step-out" from:
        // https://github.com/dotnet/runtime/blob/32d0360b73bd77256cc9a9314a3c4280a61ea9bc/src/mono/mono/component/debugger-engine.c#L1350
        std::string builderType;
        IfFailRet(TypePrinter::GetTypeOfValue(pBuilderValue, builderType));
        if (builderType == "System.Runtime.CompilerServices.AsyncVoidMethodBuilder")
            return m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OUT);

        IfFailRet(SetNotificationForWaitCompletion(pThread, pFrame, pBuilderValue, m_sharedEvalHelpers.get()));
        IfFailRet(SetBreakpointIntoNotifyDebuggerOfWaitCompletion());
        // Note, we don't create stepper here, since all we need in case of breakpoint is call Continue() from StepCommand().
        return S_OK;
    }

    AsyncInfo::AwaitInfo *awaitInfo = nullptr;
    if (m_uniqueAsyncInfo->FindNextAwaitInfo(modAddress, methodToken, methodVersion, ipOffset, &awaitInfo))
    {
        // We have step inside async function with await, setup breakpoint at closest await's yield_offset.
        // Two possible cases here:
        // 1. Step finished successful - await code not reached.
        // 2. Breakpoint was reached - step reached await block, so, we must switch to async step logic instead.

        const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);

        m_asyncStep.reset(new asyncStep_t());
        m_asyncStep->m_threadId = getThreadId(pThread);
        m_asyncStep->m_initialStepType = stepType;
        m_asyncStep->m_resume_offset = awaitInfo->resume_offset;
        m_asyncStep->m_stepStatus = asyncStepStatus::yield_offset_breakpoint;

        m_asyncStep->m_Breakpoint.reset(new asyncBreakpoint_t());
        m_asyncStep->m_Breakpoint->modAddress = modAddress;
        m_asyncStep->m_Breakpoint->methodToken = methodToken;
        m_asyncStep->m_Breakpoint->ilOffset = awaitInfo->yield_offset;

        ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
        IfFailRet(pCode->CreateBreakpoint(m_asyncStep->m_Breakpoint->ilOffset, &iCorFuncBreakpoint));
        IfFailRet(iCorFuncBreakpoint->Activate(TRUE));
        m_asyncStep->m_Breakpoint->iCorFuncBreakpoint = iCorFuncBreakpoint.Detach();
    }

    return S_FALSE; // setup simple stepper
}

HRESULT AsyncStepper::ManagedCallbackStepComplete()
{
    // In case we have async method and first await breakpoint (yield_offset) was enabled, but not reached.
    m_asyncStepMutex.lock();
    if (m_asyncStep)
        m_asyncStep.reset(nullptr);
    m_asyncStepMutex.unlock();

    return S_FALSE; // S_FALSE - no error, but steppers not affect on callback
}

HRESULT AsyncStepper::DisableAllSteppers()
{
    m_asyncStepMutex.lock();
    if (m_asyncStep)
        m_asyncStep.reset(nullptr);
    if (m_asyncStepNotifyDebuggerOfWaitCompletion)
        m_asyncStepNotifyDebuggerOfWaitCompletion.reset(nullptr);
    m_asyncStepMutex.unlock();

    return S_OK;
}

// Setup breakpoint into System.Threading.Tasks.Task.NotifyDebuggerOfWaitCompletion() method, that will be
// called at wait completion if notification was enabled by SetNotificationForWaitCompletion().
// Note, NotifyDebuggerOfWaitCompletion() will be called only once, since notification flag
// will be automatically disabled inside NotifyDebuggerOfWaitCompletion() method itself.
HRESULT AsyncStepper::SetBreakpointIntoNotifyDebuggerOfWaitCompletion()
{
    HRESULT Status = S_OK;
    static const std::string assemblyName = "System.Private.CoreLib.dll";
    static const WCHAR className[] = W("System.Threading.Tasks.Task");
    static const WCHAR methodName[] = W("NotifyDebuggerOfWaitCompletion");
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(m_sharedEvalHelpers->FindMethodInModule(assemblyName, className, methodName, &pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));
    mdMethodDef methodDef;
    IfFailRet(pFunc->GetToken(&methodDef));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));

    ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(0, &iCorFuncBreakpoint));
    IfFailRet(iCorFuncBreakpoint->Activate(TRUE));

    const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);
    m_asyncStepNotifyDebuggerOfWaitCompletion.reset(new asyncBreakpoint_t());
    m_asyncStepNotifyDebuggerOfWaitCompletion->iCorFuncBreakpoint = iCorFuncBreakpoint.Detach();
    m_asyncStepNotifyDebuggerOfWaitCompletion->modAddress = modAddress;
    m_asyncStepNotifyDebuggerOfWaitCompletion->methodToken = methodDef;

    return S_OK;
}

// Check if breakpoint is part of async stepping routine and do next action for async stepping if need.
// [in] pThread - object that represents the thread that contains the breakpoint.
HRESULT AsyncStepper::ManagedCallbackBreakpoint(ICorDebugThread *pThread)
{
    ToRelease<ICorDebugFrame> pFrame;
    mdMethodDef methodToken;
    if (FAILED(pThread->GetActiveFrame(&pFrame)) ||
        pFrame == nullptr ||
        FAILED(pFrame->GetFunctionToken(&methodToken)))
    {
        LOGE("Failed receive function token for async step");
        return S_FALSE;
    }
    CORDB_ADDRESS modAddress;
    ToRelease<ICorDebugFunction> pFunc;
    ToRelease<ICorDebugModule> pModule;
    if (FAILED(pFrame->GetFunction(&pFunc)) ||
        FAILED(pFunc->GetModule(&pModule)) ||
        FAILED(pModule->GetBaseAddress(&modAddress)))
    {
        LOGE("Failed receive module address for async step");
        return S_FALSE;
    }

    const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);

    if (!m_asyncStep)
    {
        // Care special case here, when we step-out from async method with await blocks
        // and NotifyDebuggerOfWaitCompletion magic happens with breakpoint in this method.
        // Note, if we hit NotifyDebuggerOfWaitCompletion breakpoint, it's our no matter which thread.

        if (!m_asyncStepNotifyDebuggerOfWaitCompletion ||
            modAddress != m_asyncStepNotifyDebuggerOfWaitCompletion->modAddress ||
            methodToken != m_asyncStepNotifyDebuggerOfWaitCompletion->methodToken)
            return S_FALSE;

        m_asyncStepNotifyDebuggerOfWaitCompletion.reset(nullptr);
        // Note, notification flag will be reseted automatically in NotifyDebuggerOfWaitCompletion() method,
        // no need call SetNotificationForWaitCompletion() with FALSE arg (at least, mono acts in the same way).

        // Update stepping request to new thread/frame_count that we are continuing on
        // so continuing with normal step-out works as expected.
        m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OUT);
        return S_OK;
    }

    if (modAddress != m_asyncStep->m_Breakpoint->modAddress ||
        methodToken != m_asyncStep->m_Breakpoint->methodToken)
    {
        // Async step was breaked by another breakpoint, remove async step related breakpoint.
        // Same behavior as MS vsdbg have for stepping interrupted by breakpoint.
        m_asyncStep.reset(nullptr);
        return S_FALSE;
    }

    ToRelease<ICorDebugILFrame> pILFrame;
    ULONG32 ipOffset;
    CorDebugMappingResult mappingResult;
    if (FAILED(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame)) ||
        FAILED(pILFrame->GetIP(&ipOffset, &mappingResult)))
    {
        LOGE("Failed receive current IP offset for async step");
        return S_FALSE;
    }

    if (ipOffset != m_asyncStep->m_Breakpoint->ilOffset)
    {
        // Async step was breaked by another breakpoint, remove async step related breakpoint.
        // Same behavior as MS vsdbg have for stepping interrupted by breakpoint.
        m_asyncStep.reset(nullptr);
        return S_FALSE;
    }

    if (m_asyncStep->m_stepStatus == asyncStepStatus::yield_offset_breakpoint)
    {
        // Note, in case of first breakpoint for async step, we must have same thread.
        if (m_asyncStep->m_threadId != getThreadId(pThread))
        {
            // Parallel thread execution, skip it and continue async step routine.
            return S_OK;
        }

        HRESULT Status;
        ToRelease<ICorDebugProcess> pProcess;
        IfFailRet(pThread->GetProcess(&pProcess));
        m_simpleStepper->DisableAllSteppers(pProcess);

        m_asyncStep->m_stepStatus = asyncStepStatus::resume_offset_breakpoint;

        ToRelease<ICorDebugCode> pCode;
        ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
        if (FAILED(pFunc->GetILCode(&pCode)) ||
            FAILED(pCode->CreateBreakpoint(m_asyncStep->m_resume_offset, &iCorFuncBreakpoint)) ||
            FAILED(iCorFuncBreakpoint->Activate(TRUE)))
        {
            LOGE("Could not setup second breakpoint (resume_offset) for await block");
            return S_FALSE;
        }

        m_asyncStep->m_Breakpoint->iCorFuncBreakpoint->Activate(FALSE);
        m_asyncStep->m_Breakpoint->iCorFuncBreakpoint = iCorFuncBreakpoint.Detach();
        m_asyncStep->m_Breakpoint->ilOffset = m_asyncStep->m_resume_offset;

        CorDebugHandleType handleType;
        ToRelease<ICorDebugValue> iCorValue;
        if (FAILED(GetAsyncIdReference(pThread, pFrame, m_sharedEvalHelpers.get(), &iCorValue)) ||
            FAILED(iCorValue->QueryInterface(IID_ICorDebugHandleValue , (LPVOID*) &m_asyncStep->m_iCorHandleValueAsyncId)) ||
            FAILED(m_asyncStep->m_iCorHandleValueAsyncId->GetHandleType(&handleType)) ||
            handleType != HANDLE_STRONG) // Note, we need only strong handle here, that will not invalidated on continue-break.
        {
            m_asyncStep->m_iCorHandleValueAsyncId.Free();
            LOGE("Could not setup handle with async ID for await block");
        }
    }
    else
    {
        // For second breakpoint we could have 3 cases:
        // 1. We still have initial thread, so, no need spend time and check asyncId.
        // 2. We have another thread with same asyncId - same execution of async method.
        // 3. We have another thread with different asyncId - parallel execution of async method.
        if (m_asyncStep->m_threadId == getThreadId(pThread))
        {
            m_simpleStepper->SetupStep(pThread, m_asyncStep->m_initialStepType);
            m_asyncStep.reset(nullptr);
            return S_OK;
        }

        ToRelease<ICorDebugValue> pValueRef;
        CORDB_ADDRESS currentAsyncId = 0;
        ToRelease<ICorDebugValue> pValue;
        BOOL isNull = FALSE;
        if (SUCCEEDED(GetAsyncIdReference(pThread, pFrame, m_sharedEvalHelpers.get(), &pValueRef)) &&
            SUCCEEDED(DereferenceAndUnboxValue(pValueRef, &pValue, &isNull)) && !isNull)
            pValue->GetAddress(&currentAsyncId);
        else
            LOGE("Could not calculate current async ID for await block");

        CORDB_ADDRESS prevAsyncId = 0;
        ToRelease<ICorDebugValue> pDereferencedValue;
        ToRelease<ICorDebugValue> pValueAsyncId;
        if (m_asyncStep->m_iCorHandleValueAsyncId && // Note, we could fail with m_iCorHandleValueAsyncId on previous breakpoint by some reason.
            SUCCEEDED(m_asyncStep->m_iCorHandleValueAsyncId->Dereference(&pDereferencedValue)) &&
            SUCCEEDED(DereferenceAndUnboxValue(pDereferencedValue, &pValueAsyncId, &isNull)) && !isNull)
            pValueAsyncId->GetAddress(&prevAsyncId);
        else
            LOGE("Could not calculate previous async ID for await block");

        // Note, 'currentAsyncId' and 'prevAsyncId' is 64 bit addresses, in our case can't be 0.
        // If we can't detect proper thread - continue stepping for this thread.
        if (currentAsyncId == prevAsyncId || currentAsyncId == 0 || prevAsyncId == 0)
        {
            m_simpleStepper->SetupStep(pThread, m_asyncStep->m_initialStepType);
            m_asyncStep.reset(nullptr);
        }
    }

    return S_OK;
}

} // namespace netcoredbg
