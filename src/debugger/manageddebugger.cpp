// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <vector>
#include <map>

#include <sys/types.h>
#include <sys/stat.h>
#include "debugger/dbgshim.h"
#include "debugger/manageddebugger.h"
#include "debugger/managedcallback.h"
#include "valueprint.h"
#include "managed/interop.h"
#include "utils/utf.h"
#include "dynlibs.h"
#include "metadata/typeprinter.h"
#include "debugger/frames.h"
#include "utils/logger.h"
#include "debugger/waitpid.h"
#include "iosystem.h"

#include "palclr.h"

using std::string;
using std::vector;
using std::map;

namespace netcoredbg
{

#ifdef FEATURE_PAL

// as alternative, libuuid should be linked...
// (the problem is, that in CoreClr > 3.x, in pal/inc/rt/rpc.h,
// MIDL_INTERFACE uses DECLSPEC_UUID, which has empty definition.
extern "C" const IID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 }};

#endif // FEATURE_PAL

namespace 
{
    int GetSystemEnvironmentAsMap(map<string, string>& outMap)
    {
        char*const*const pEnv = GetSystemEnvironment();

        if (pEnv == nullptr)
            return -1;

        int counter = 0;
        while (pEnv[counter] != nullptr)
        {
            const string env = pEnv[counter];
            size_t pos = env.find_first_of("=");
            if (pos != string::npos && pos != 0)
                outMap.emplace(env.substr(0, pos), env.substr(pos+1));

            ++counter;
        }

        return 0;
    }
}

dbgshim_t g_dbgshim;

ThreadId getThreadId(ICorDebugThread *pThread)
{
    DWORD threadId = 0;  // invalid value for Win32
    HRESULT res = pThread->GetID(&threadId);
    return res == S_OK && threadId != 0 ? ThreadId{threadId} : ThreadId{};
}

void ManagedDebugger::NotifyProcessCreated()
{
    std::lock_guard<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessAttached;
}

void ManagedDebugger::NotifyProcessExited()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    m_processAttachedState = ProcessUnattached;
    lock.unlock();
    m_processAttachedCV.notify_one();
}

void ManagedDebugger::WaitProcessExited()
{
    std::unique_lock<std::mutex> lock(m_processAttachedMutex);
    if (m_processAttachedState != ProcessUnattached)
        m_processAttachedCV.wait(lock, [this]{return m_processAttachedState == ProcessUnattached;});
}

static HRESULT DisableAllSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;
    ToRelease<ICorDebugStepperEnum> steppers;
    IfFailRet(pAppDomain->EnumerateSteppers(&steppers));

    ICorDebugStepper *curStepper;
    ULONG steppersFetched;
    while (SUCCEEDED(steppers->Next(1, &curStepper, &steppersFetched)) && steppersFetched == 1)
    {
        ToRelease<ICorDebugStepper> pStepper(curStepper);
        pStepper->Deactivate();
    }

    return S_OK;
}

HRESULT ManagedDebugger::DisableAllSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

static HRESULT DisableAllBreakpointsAndSteppersInAppDomain(ICorDebugAppDomain *pAppDomain)
{
    ToRelease<ICorDebugBreakpointEnum> breakpoints;
    if (SUCCEEDED(pAppDomain->EnumerateBreakpoints(&breakpoints)))
    {
        ICorDebugBreakpoint *curBreakpoint;
        ULONG breakpointsFetched;
        while (SUCCEEDED(breakpoints->Next(1, &curBreakpoint, &breakpointsFetched)) && breakpointsFetched == 1)
        {
            ToRelease<ICorDebugBreakpoint> pBreakpoint(curBreakpoint);
            pBreakpoint->Activate(FALSE);
        }
    }

    DisableAllSteppersInAppDomain(pAppDomain);

    return S_OK;
}

static HRESULT DisableAllBreakpointsAndSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;

    ToRelease<ICorDebugAppDomainEnum> domains;
    IfFailRet(pProcess->EnumerateAppDomains(&domains));

    ICorDebugAppDomain *curDomain;
    ULONG domainsFetched;
    while (SUCCEEDED(domains->Next(1, &curDomain, &domainsFetched)) && domainsFetched == 1)
    {
        ToRelease<ICorDebugAppDomain> pDomain(curDomain);
        DisableAllBreakpointsAndSteppersInAppDomain(pDomain);
    }
    return S_OK;
}

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
// [in] evaluator - reference to managed debugger evaluator;
// [out] ppValueAsyncIdRef - result value (reference to created by builder object).
static HRESULT GetAsyncIdReference(ICorDebugThread *pThread, ICorDebugFrame *pFrame, Evaluator &evaluator, ICorDebugValue **ppValueAsyncIdRef)
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
    IfFailRet(evaluator.EvalFunction(pThread, pFunc, pType.GetRef(), 1, pValue.GetRef(), 1, ppValueAsyncIdRef, defaultEvalFlags));

    return S_OK;
}

// Set notification for wait completion - call SetNotificationForWaitCompletion() method for particular builder.
// [in] pThread - managed thread for evaluation (related to pFrame);
// [in] pFrame - frame that used for get all info needed (function, module, etc);
// [in] evaluator - reference to managed debugger evaluator;
static HRESULT SetNotificationForWaitCompletion(ICorDebugThread *pThread, ICorDebugFrame *pFrame, Evaluator &evaluator)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> pValue;
    IfFailRet(GetAsyncTBuilder(pFrame, &pValue));

    // Find SetNotificationForWaitCompletion() method.
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
    ToRelease<ICorDebugValue2> pNewBooleanValue2;
    IfFailRet(pNewBoolean->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pNewBooleanValue2));
    ToRelease<ICorDebugType> pNewBooleanType;
    IfFailRet(pNewBooleanValue2->GetExactType(&pNewBooleanType));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(setNotifDef, &pFunc));

    ICorDebugType *ppArgsType[] = {pType, pNewBooleanType};
    ICorDebugValue *ppArgsValue[] = {pValue, pNewBoolean};
    IfFailRet(evaluator.EvalFunction(pThread, pFunc, ppArgsType, 2, ppArgsValue, 2, nullptr, defaultEvalFlags));

    return S_OK;
}

// Setup breakpoint into System.Threading.Tasks.Task.NotifyDebuggerOfWaitCompletion() method, that will be
// called at wait completion if notification was enabled by SetNotificationForWaitCompletion().
// Note, NotifyDebuggerOfWaitCompletion() will be called only once, since notification flag
// will be automatically disabled inside NotifyDebuggerOfWaitCompletion() method itself.
HRESULT ManagedDebugger::SetBreakpointIntoNotifyDebuggerOfWaitCompletion()
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(m_modules.GetModuleWithName("System.Private.CoreLib.dll", &pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDef[] = W("System.Threading.Tasks.Task");
    IfFailRet(pMD->FindTypeDefByName(strTypeDef, mdTypeDefNil, &typeDef));

    ULONG numMethods = 0;
    HCORENUM hEnum = NULL;
    mdMethodDef methodDef;
    mdMethodDef notifyDef = mdMethodDefNil;
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

        if (!str_equal(szFunctionName, W("NotifyDebuggerOfWaitCompletion")))
            continue;

        notifyDef = methodDef;
        break;
    }
    pMD->CloseEnum(hEnum);

    if (notifyDef == mdMethodDefNil)
        return E_FAIL;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pModule->GetFunctionFromToken(notifyDef, &pFunc));

    ToRelease<ICorDebugCode> pCode;
    IfFailRet(pFunc->GetILCode(&pCode));

    ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
    IfFailRet(pCode->CreateBreakpoint(0, &pBreakpoint));
    IfFailRet(pBreakpoint->Activate(TRUE));

    const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);
    modAddressNotifyDebuggerOfWaitCompletion = modAddress;
    methodTokenNotifyDebuggerOfWaitCompletion = notifyDef;

    return S_OK;
}

void ManagedDebugger::SetLastStoppedThread(ICorDebugThread *pThread)
{
    SetLastStoppedThreadId(getThreadId(pThread));
}

void ManagedDebugger::SetLastStoppedThreadId(ThreadId threadId)
{
    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    m_lastStoppedThreadId = threadId;
}

void ManagedDebugger::InvalidateLastStoppedThreadId()
{
    SetLastStoppedThreadId(ThreadId::AllThreads);
}

ThreadId ManagedDebugger::GetLastStoppedThreadId()
{
    LogFuncEntry();

    std::lock_guard<std::mutex> lock(m_lastStoppedThreadIdMutex);
    return m_lastStoppedThreadId;
}

ManagedDebugger::ManagedDebugger() :
    m_processAttachedState(ProcessUnattached),
    m_lastStoppedThreadId(ThreadId::AllThreads),
    m_stopCounter(0),
    m_startMethod(StartNone),
    m_stopAtEntry(false),
    m_isConfigurationDone(false),
    m_evaluator(m_modules),
    m_breakpoints(m_modules),
    m_variables(m_evaluator),
    m_protocol(nullptr),
    m_managedCallback(new ManagedCallback(*this)),
    m_pDebug(nullptr),
    m_pProcess(nullptr),
    m_justMyCode(true),
    m_startupReady(false),
    m_startupResult(S_OK),
    m_unregisterToken(nullptr),
    m_processId(0),
    m_ioredirect(
        { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
        std::bind(&ManagedDebugger::InputCallback, this, std::placeholders::_1, std::placeholders::_2)
    ),
    m_asyncStep(nullptr)
{
}

ManagedDebugger::~ManagedDebugger()
{
}

HRESULT ManagedDebugger::Initialize()
{
    LogFuncEntry();

    // TODO: Report capabilities and check client support
    m_startMethod = StartNone;
    m_protocol->EmitInitializedEvent();
    return S_OK;
}

HRESULT ManagedDebugger::RunIfReady()
{
    FrameId::invalidate();

    if (m_startMethod == StartNone || !m_isConfigurationDone)
        return S_OK;

    switch(m_startMethod)
    {
        case StartLaunch:
            return RunProcess(m_execPath, m_execArgs);
        case StartAttach:
            return AttachToProcess(m_processId);
        default:
            return E_FAIL;
    }

    //Unreachable
    return E_FAIL;
}

HRESULT ManagedDebugger::Attach(int pid)
{
    LogFuncEntry();

    m_startMethod = StartAttach;
    m_processId = pid;
    return RunIfReady();
}

HRESULT ManagedDebugger::Launch(const string &fileExec, const vector<string> &execArgs, const map<string, string> &env, const string &cwd, bool stopAtEntry)
{
    LogFuncEntry();

    m_startMethod = StartLaunch;
    m_execPath = fileExec;
    m_execArgs = execArgs;
    m_stopAtEntry = stopAtEntry;
    m_cwd = cwd;
    m_env = env;
    m_breakpoints.SetStopAtEntry(m_stopAtEntry);
    return RunIfReady();
}

HRESULT ManagedDebugger::ConfigurationDone()
{
    LogFuncEntry();

    m_isConfigurationDone = true;

    return RunIfReady();
}

HRESULT ManagedDebugger::Disconnect(DisconnectAction action)
{
    LogFuncEntry();

    bool terminate;
    switch(action)
    {
        case DisconnectDefault:
            switch(m_startMethod)
            {
                case StartLaunch:
                    terminate = true;
                    break;
                case StartAttach:
                    terminate = false;
                    break;
                default:
                    return E_FAIL;
            }
            break;
        case DisconnectTerminate:
            terminate = true;
            break;
        case DisconnectDetach:
            terminate = false;
            break;
        default:
            return E_FAIL;
    }

    if (!terminate)
    {
        HRESULT Status = DetachFromProcess();
        if (SUCCEEDED(Status))
            m_protocol->EmitTerminatedEvent();
        return Status;
    }

    return TerminateProcess();
}

// Common method to setup step.
// [in] pThread - managed thread for stepping setup;
// [in] stepType - step type.
HRESULT ManagedDebugger::SetupStep(ICorDebugThread *pThread, StepType stepType)
{
    HRESULT Status;

    m_asyncStepMutex.lock();
    if (m_asyncStep)
        m_asyncStep.reset(nullptr);
    m_asyncStepMutex.unlock();

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

    if (!m_modules.IsMethodHaveAwait(modAddress, methodToken))
        return SetupSimpleStep(pThread, stepType);

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 ipOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ipOffset, &mappingResult));

    // If we are at end of async method with await blocks and doing step-in or step-over,
    // switch to step-out, so whole NotifyDebuggerOfWaitCompletion magic happens.
    ULONG32 lastIlOffset;
    if (stepType != Debugger::StepType::STEP_OUT &&
        m_modules.FindLastIlOffsetAwaitInfo(modAddress, methodToken, lastIlOffset) &&
        ipOffset >= lastIlOffset)
    {
        stepType = Debugger::StepType::STEP_OUT;
    }
    if (stepType == Debugger::StepType::STEP_OUT)
    {
        IfFailRet(SetNotificationForWaitCompletion(pThread, pFrame, m_evaluator));
        IfFailRet(SetBreakpointIntoNotifyDebuggerOfWaitCompletion());
        // Note, we don't create stepper here, since all we need in case of breakpoint is call Continue() from StepCommand().
        return S_OK;
    }

    Modules::AwaitInfo *awaitInfo = nullptr;
    if (m_modules.FindNextAwaitInfo(modAddress, methodToken, ipOffset, &awaitInfo))
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

        ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
        IfFailRet(pCode->CreateBreakpoint(m_asyncStep->m_Breakpoint->ilOffset, &pBreakpoint));
        IfFailRet(pBreakpoint->Activate(TRUE));
        m_asyncStep->m_Breakpoint->iCorBreakpoint = pBreakpoint.Detach();
    }

    return SetupSimpleStep(pThread, stepType);
}

// Setup simple step with provided by runtime ICorDebugStepper interface.
// [in] pThread - managed thread for stepping setup;
// [in] stepType - step type.
HRESULT ManagedDebugger::SetupSimpleStep(ICorDebugThread *pThread, StepType stepType)
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

    IfFailRet(pStepper2->SetJMC(IsJustMyCode()));

    ThreadId threadId(getThreadId(pThread));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());

        std::lock_guard<std::mutex> lock(m_stepMutex);
        m_stepSettedUp[int(threadId)] = true;

        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(m_modules.GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    std::lock_guard<std::mutex> lock(m_stepMutex);
    m_stepSettedUp[int(threadId)] = true;

    return S_OK;
}

HRESULT ManagedDebugger::StepCommand(ThreadId threadId, StepType stepType)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;
    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(int(threadId), &pThread));
    DisableAllSteppers(m_pProcess);
    IfFailRet(SetupStep(pThread, stepType));

    m_variables.Clear();
    Status = m_pProcess->Continue(0);

    if (SUCCEEDED(Status))
    {
        FrameId::invalidate();
        m_protocol->EmitContinuedEvent(threadId);
        --m_stopCounter;
    }
    return Status;
}

HRESULT ManagedDebugger::Stop(ThreadId threadId, const StoppedEvent &event)
{
    LogFuncEntry();

    HRESULT Status = S_OK;

    while (m_stopCounter.load() > 0) {
        m_protocol->EmitContinuedEvent(m_lastStoppedThreadId);
        --m_stopCounter;
    }
    // INFO: Double EmitStopEvent() produce blocked coreclr command reader
    m_stopCounter.store(1); // store zero and increment
    m_protocol->EmitStoppedEvent(event);

    return Status;
}

HRESULT ManagedDebugger::Continue(ThreadId threadId)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;

    HRESULT res = S_OK;
    if (!m_evaluator.IsEvalRunning() && m_evaluator.is_empty_eval_queue()) {
        if ((res = m_pProcess->SetAllThreadsDebugState(THREAD_RUN, nullptr)) != S_OK) {
            // TODO: need function for printing coreCLR errors by error code
            switch (res) {
                case CORDBG_E_PROCESS_NOT_SYNCHRONIZED:
                    LOGE("Setting thread state failed. Process not synchronized:'%0x'", res);
                break;
                case CORDBG_E_PROCESS_TERMINATED:
                    LOGE("Setting thread state failed. Process was terminated:'%0x'", res);
                break;
                case CORDBG_E_OBJECT_NEUTERED:
                    LOGE("Setting thread state failed. Object has been neutered(it's in a zombie state):'%0x'", res);
                break;
                default:
                    LOGE("SetAllThreadsDebugState() %0x", res);
                break;
            }
        }
    }
    if ((res = m_pProcess->Continue(0)) != S_OK) {
        switch (res) {
        case CORDBG_E_SUPERFLOUS_CONTINUE:
            LOGE("Continue failed. Returned from a call to Continue that was not matched with a stopping event:'%0x'", res);
            break;
        case CORDBG_E_PROCESS_TERMINATED:
            LOGE("Continue failed. Process was terminated:'%0x'", res);
            break;
        case CORDBG_E_OBJECT_NEUTERED:
            LOGE("Continue failed. Object has been neutered(it's in a zombie state):'%0x'", res);
            break;
        default:
            LOGE("Continue() %0x", res);
            break;
        }
    }

    if (SUCCEEDED(res))
    {
        FrameId::invalidate();
        m_protocol->EmitContinuedEvent(threadId);
        --m_stopCounter;
    }

    return res;
}

HRESULT ManagedDebugger::Pause()
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;

    // The debugger maintains a stop counter. When the counter goes to zero, the controller is resumed.
    // Each call to Stop or each dispatched callback increments the counter.
    // Each call to ICorDebugController::Continue decrements the counter.
    BOOL running = FALSE;
    HRESULT Status = m_pProcess->IsRunning(&running);
    if (Status != S_OK)
        return Status;
    if (!running)
        return S_OK;

    Status = m_pProcess->Stop(0);
    if (Status != S_OK)
        return Status;

    // Same logic as provide vsdbg in case of pause during stepping.
    m_asyncStepMutex.lock();
    if (m_asyncStep)
        m_asyncStep.reset(nullptr);
    m_asyncStepMutex.unlock();
    DisableAllSteppers(m_pProcess);

    // For Visual Studio, we have to report a thread ID in async stop event.
    // We have to find a thread which has a stack frame with valid location in its stack trace.
    std::vector<Thread> threads;
    GetThreads(threads);

    ThreadId lastStoppedId = GetLastStoppedThreadId();

    // Reorder threads so that last stopped thread is checked first
    for (size_t i = 0; i < threads.size(); ++i)
    {
        if (threads[i].id == lastStoppedId)
        {
            std::swap(threads[0], threads[i]);
            break;
        }
    }

    // Now get stack trace for each thread and find a frame with valid source location.
    for (const Thread& thread : threads)
    {
        int totalFrames = 0;
        std::vector<StackFrame> stackFrames;

        if (FAILED(GetStackTrace(thread.id, FrameLevel(0), 0, stackFrames, totalFrames)))
            continue;

        for (const StackFrame& stackFrame : stackFrames)
        {
            if (stackFrame.source.IsNull())
                continue;

            StoppedEvent event(StopPause, thread.id);
            event.frame = stackFrame;
            SetLastStoppedThreadId(thread.id);
            m_protocol->EmitStoppedEvent(event);

            return Status;
        }
    }

    m_protocol->EmitStoppedEvent(StoppedEvent(StopPause, ThreadId::Invalid));

    return Status;
}

HRESULT ManagedDebugger::GetThreads(std::vector<Thread> &threads)
{
    LogFuncEntry();

    if (!m_pProcess)
        return E_FAIL;
    return GetThreadsState(m_pProcess, threads);
}

HRESULT ManagedDebugger::GetStackTrace(ThreadId  threadId, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    HRESULT Status;
    if (!m_pProcess)
        return E_FAIL;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(int(threadId), &pThread));
    return GetStackTrace(pThread, startFrame, maxFrames, stackFrames, totalFrames);
}

VOID ManagedDebugger::StartupCallback(IUnknown *pCordb, PVOID parameter, HRESULT hr)
{
    ManagedDebugger *self = static_cast<ManagedDebugger*>(parameter);

    std::unique_lock<std::mutex> lock(self->m_startupMutex);

    self->m_startupResult = FAILED(hr) ? hr : self->Startup(pCordb, self->m_processId);
    self->m_startupReady = true;

    if (self->m_unregisterToken)
    {
        g_dbgshim.UnregisterForRuntimeStartup(self->m_unregisterToken);
        self->m_unregisterToken = nullptr;
    }

    lock.unlock();
    self->m_startupCV.notify_one();
}

// From dbgshim.cpp
static bool AreAllHandlesValid(HANDLE *handleArray, DWORD arrayLength)
{
    for (DWORD i = 0; i < arrayLength; i++)
    {
        HANDLE h = handleArray[i];
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
    }
    return true;
}

static HRESULT InternalEnumerateCLRs(
    DWORD pid, HANDLE **ppHandleArray, LPWSTR **ppStringArray, DWORD *pdwArrayLength, int tryCount)
{
    int numTries = 0;
    HRESULT hr;

    while (numTries < tryCount)
    {
        hr = g_dbgshim.EnumerateCLRs(pid, ppHandleArray, ppStringArray, pdwArrayLength);

        // From dbgshim.cpp:
        // EnumerateCLRs uses the OS API CreateToolhelp32Snapshot which can return ERROR_BAD_LENGTH or
        // ERROR_PARTIAL_COPY. If we get either of those, we try wait 1/10th of a second try again (that
        // is the recommendation of the OS API owners).
        // In dbgshim the following condition is used:
        //  if ((hr != HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY)) && (hr != HRESULT_FROM_WIN32(ERROR_BAD_LENGTH)))
        // Since we may be attaching to the process which has not loaded coreclr yes, let's give it some time to load.
        if (SUCCEEDED(hr))
        {
            // Just return any other error or if no handles were found (which means the coreclr module wasn't found yet).
            if (*ppHandleArray != NULL && *pdwArrayLength > 0)
            {

                // If EnumerateCLRs succeeded but any of the handles are INVALID_HANDLE_VALUE, then sleep and retry
                // also. This fixes a race condition where dbgshim catches the coreclr module just being loaded but
                // before g_hContinueStartupEvent has been initialized.
                if (AreAllHandlesValid(*ppHandleArray, *pdwArrayLength))
                {
                    return hr;
                }
                // Clean up memory allocated in EnumerateCLRs since this path it succeeded
                g_dbgshim.CloseCLREnumeration(*ppHandleArray, *ppStringArray, *pdwArrayLength);

                *ppHandleArray = NULL;
                *ppStringArray = NULL;
                *pdwArrayLength = 0;
            }
        }

        // No point in retrying in case of invalid arguments or no such process
        if (hr == E_INVALIDARG || hr == E_FAIL)
            return hr;

        // Sleep and retry enumerating the runtimes
        USleep(100*1000);
        numTries++;

        // if (m_canceled)
        // {
        //     break;
        // }
    }

    // Indicate a timeout
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    return hr;
}

static string GetCLRPath(DWORD pid, int timeoutSec = 3)
{
    HANDLE* pHandleArray;
    LPWSTR* pStringArray;
    DWORD dwArrayLength;
    const int tryCount = timeoutSec * 10; // 100ms interval between attempts
    if (FAILED(InternalEnumerateCLRs(pid, &pHandleArray, &pStringArray, &dwArrayLength, tryCount)) || dwArrayLength == 0)
        return string();

    string result = to_utf8(pStringArray[0]);

    g_dbgshim.CloseCLREnumeration(pHandleArray, pStringArray, dwArrayLength);

    return result;
}

HRESULT ManagedDebugger::Startup(IUnknown *punk, DWORD pid)
{
    LogFuncEntry();

    HRESULT Status;

    ToRelease<ICorDebug> pCorDebug;
    IfFailRet(punk->QueryInterface(IID_ICorDebug, (void **)&pCorDebug));

    IfFailRet(pCorDebug->Initialize());

    Status = pCorDebug->SetManagedHandler(m_managedCallback);
    if (FAILED(Status))
    {
        pCorDebug->Terminate();
        return Status;
    }

    if (m_clrPath.empty())
        m_clrPath = GetCLRPath(pid);

    ManagedPart::SetCoreCLRPath(m_clrPath);

    ToRelease<ICorDebugProcess> pProcess;
    Status = pCorDebug->DebugActiveProcess(pid, FALSE, &pProcess);
    if (FAILED(Status))
    {
        pCorDebug->Terminate();
        return Status;
    }

    m_pProcess = pProcess.Detach();
    m_pDebug = pCorDebug.Detach();

    m_processId = pid;

#ifdef FEATURE_PAL
    GetWaitpid().SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

    return S_OK;
}

static string EscapeShellArg(const string &arg)
{
    string s(arg);

    for (string::size_type i = 0; i < s.size(); ++i)
    {
        string::size_type count = 0;
        char c = s.at(i);
        switch (c)
        {
            case '\"': count = 1; s.insert(i, count, '\\'); s[i + count] = '\"'; break;
            case '\\': count = 1; s.insert(i, count, '\\'); s[i + count] = '\\'; break;
        }
        i += count;
    }

    return s;
}

static bool IsDirExists(const char* const path)
{
    struct stat info;

    if (stat(path, &info) != 0)
        return false;

    if (!(info.st_mode & S_IFDIR))
        return false;

    return true;
}

HRESULT ManagedDebugger::RunProcess(const string& fileExec, const std::vector<string>& execArgs)
{
    static const auto startupCallbackWaitTimeout = std::chrono::milliseconds(5000);
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    std::ostringstream ss;
    ss << "\"" << fileExec << "\"";
    for (const string &arg : execArgs)
        ss << " \"" << EscapeShellArg(arg) << "\"";

    m_startupReady = false;
    m_clrPath.clear();

    HANDLE resumeHandle; // Fake thread handle for the process resume

    vector<char> outEnv;
    if (!m_env.empty()) {
        // We need to append the environ values with keeping the current process environment block.
        // It works equal for any platrorms in coreclr CreateProcessW(), but not critical for Linux.
        map<string, string> envMap;
        if (GetSystemEnvironmentAsMap(envMap) != -1) {
            auto it = m_env.begin();
            auto end = m_env.end();
            // Override the system value (PATHs appending needs a complex implementation)
            while (it != end) {
                envMap[it->first] = it->second;
                ++it;
            }
            for (const auto &pair : envMap) {
                outEnv.insert(outEnv.end(), pair.first.begin(), pair.first.end());
                outEnv.push_back('=');
                outEnv.insert(outEnv.end(), pair.second.begin(), pair.second.end());
                outEnv.push_back('\0');
            }
            outEnv.push_back('\0');
        } else {
            for (const auto &pair : m_env) {
                outEnv.insert(outEnv.end(), pair.first.begin(), pair.first.end());
                outEnv.push_back('=');
                outEnv.insert(outEnv.end(), pair.second.begin(), pair.second.end());
                outEnv.push_back('\0');
            }
        }
    }

    // cwd in launch.json set working directory for debugger https://code.visualstudio.com/docs/python/debugging#_cwd
    if (!m_cwd.empty())
        if (!IsDirExists(m_cwd.c_str()) || !SetWorkDir(m_cwd))
            m_cwd.clear();

    Status = m_ioredirect.exec([&]() -> HRESULT {
            IfFailRet(g_dbgshim.CreateProcessForLaunch(reinterpret_cast<LPWSTR>(const_cast<WCHAR*>(to_utf16(ss.str()).c_str())),
                                     /* Suspend process */ TRUE,
                                     outEnv.empty() ? NULL : &outEnv[0],
                                     m_cwd.empty() ? NULL : reinterpret_cast<LPCWSTR>(to_utf16(m_cwd).c_str()),
                                     &m_processId, &resumeHandle));
            return Status;
        });

    if (FAILED(Status))
        return Status;

#ifdef FEATURE_PAL
    GetWaitpid().SetupTrackingPID(m_processId);
#endif // FEATURE_PAL

    IfFailRet(g_dbgshim.RegisterForRuntimeStartup(m_processId, ManagedDebugger::StartupCallback, this, &m_unregisterToken));

    // Resume the process so that StartupCallback can run
    IfFailRet(g_dbgshim.ResumeProcess(resumeHandle));
    g_dbgshim.CloseResumeHandle(resumeHandle);

    // Wait for ManagedDebugger::StartupCallback to complete

    /// FIXME: if the process exits too soon the ManagedDebugger::StartupCallback()
    /// is never called (bug in dbgshim?).
    /// The workaround is to wait with timeout.
    const auto now = std::chrono::system_clock::now();

    std::unique_lock<std::mutex> lock(m_startupMutex);
    if (!m_startupCV.wait_until(lock, now + startupCallbackWaitTimeout, [this](){return m_startupReady;}))
    {
        // Timed out
        g_dbgshim.UnregisterForRuntimeStartup(m_unregisterToken);
        m_unregisterToken = nullptr;
        return E_FAIL;
    }

    if (m_startupResult == S_OK)
        m_protocol->EmitExecEvent(PID{m_processId}, fileExec);

    return m_startupResult;
}

HRESULT ManagedDebugger::CheckNoProcess()
{
    if (m_pProcess || m_pDebug)
    {
        std::unique_lock<std::mutex> lock(m_processAttachedMutex);
        if (m_processAttachedState == ProcessAttached)
            return E_FAIL; // Already attached
        lock.unlock();

        TerminateProcess();
    }
    return S_OK;
}

HRESULT ManagedDebugger::DetachFromProcess()
{
    if (!m_pProcess || !m_pDebug)
        return E_FAIL;

    if (SUCCEEDED(m_pProcess->Stop(0)))
    {
        m_breakpoints.DeleteAllBreakpoints();
        m_asyncStepMutex.lock();
        if (m_asyncStep)
            m_asyncStep.reset(nullptr);
        m_asyncStepMutex.unlock();
        DisableAllBreakpointsAndSteppers(m_pProcess);
        m_pProcess->Detach();
    }

    Cleanup();

    m_pProcess->Release();
    m_pProcess = nullptr;

    m_pDebug->Terminate();
    m_pDebug = nullptr;

    return S_OK;
}

HRESULT ManagedDebugger::TerminateProcess()
{
    if (!m_pProcess || !m_pDebug)
        return E_FAIL;

    if (SUCCEEDED(m_pProcess->Stop(0)))
    {
        m_asyncStepMutex.lock();
        if (m_asyncStep)
            m_asyncStep.reset(nullptr);
        m_asyncStepMutex.unlock();
        DisableAllBreakpointsAndSteppers(m_pProcess);
        //pProcess->Detach();
    }

    Cleanup();

    m_pProcess->Terminate(0);
    WaitProcessExited();

    m_pProcess->Release();
    m_pProcess = nullptr;

    m_pDebug->Terminate();
    m_pDebug = nullptr;

    return S_OK;
}

void ManagedDebugger::Cleanup()
{
    m_modules.CleanupAllModules();
    m_evaluator.Cleanup();
    m_protocol->Cleanup();
    // TODO: Cleanup libcoreclr.so instance
}

HRESULT ManagedDebugger::AttachToProcess(DWORD pid)
{
    HRESULT Status;

    IfFailRet(CheckNoProcess());

    m_clrPath = GetCLRPath(pid);
    if (m_clrPath.empty())
        return E_INVALIDARG; // Unable to find libcoreclr.so

    WCHAR pBuffer[100];
    DWORD dwLength;
    IfFailRet(g_dbgshim.CreateVersionStringFromModule(
        pid,
        reinterpret_cast<LPCWSTR>(to_utf16(m_clrPath).c_str()),
        pBuffer,
        _countof(pBuffer),
        &dwLength));

    ToRelease<IUnknown> pCordb;

    IfFailRet(g_dbgshim.CreateDebuggingInterfaceFromVersionEx(CorDebugVersion_4_0, pBuffer, &pCordb));

    m_unregisterToken = nullptr;
    return Startup(pCordb, pid);
}

// VSCode
HRESULT ManagedDebugger::GetExceptionInfoResponse(ThreadId threadId,
    ExceptionInfoResponse &exceptionInfoResponse)
{
    LogFuncEntry();

    // Are needed to move next line to Exception() callback?
    assert(int(threadId) != -1);
    m_evaluator.push_eval_queue(threadId);

    HRESULT res = E_FAIL;
    HRESULT Status = S_OK;
    bool hasInner = false;
    Variable varException;
    ToRelease <ICorDebugValue> evalValue;
    ToRelease<ICorDebugValue> pExceptionValue;
    ToRelease<ICorDebugThread> pThread;

    WCHAR message[] = W("_message\0");
    FrameId frameId;

    std::unique_lock<std::mutex> lock(m_lastUnhandledExceptionThreadIdsMutex);
    if (m_lastUnhandledExceptionThreadIds.find(threadId) != m_lastUnhandledExceptionThreadIds.end()) {
        lock.unlock();
        exceptionInfoResponse.breakMode.resetAll();
    }
    else {
        lock.unlock();
        ExceptionBreakMode mode;

        if ((res = m_breakpoints.GetExceptionBreakMode(mode, "*")) && FAILED(res))
            goto failed;

        exceptionInfoResponse.breakMode = mode;
    }

    if ((res = m_pProcess->GetThread(int(threadId), &pThread)) && FAILED(res))
        goto failed;

    if ((res = pThread->GetCurrentException(&pExceptionValue)) && FAILED(res)) {
        LOGE("GetCurrentException() failed, %0x", res);
        goto failed;
    }

    PrintStringField(pExceptionValue, message, exceptionInfoResponse.description);

    if ((res = m_variables.GetExceptionVariable(frameId, pThread, varException)) && FAILED(res))
        goto failed;

    if (exceptionInfoResponse.breakMode.OnlyUnhandled() || exceptionInfoResponse.breakMode.UserUnhandled()) {
        exceptionInfoResponse.description = "An unhandled exception of type '" + varException.type +
            "' occurred in " + varException.module;
    }
    else {
        exceptionInfoResponse.description = "Exception thrown: '" + varException.type +
            "' in " + varException.module;
    }

    exceptionInfoResponse.exceptionId = varException.type;

    exceptionInfoResponse.details.evaluateName = varException.name;
    exceptionInfoResponse.details.typeName = varException.type;
    exceptionInfoResponse.details.fullTypeName = varException.type;

    if (FAILED(m_evaluator.getObjectByFunction("get_StackTrace", pThread, pExceptionValue, &evalValue, defaultEvalFlags))) {
        exceptionInfoResponse.details.stackTrace = "<undefined>"; // Evaluating problem entire object
    }
    else {
        PrintValue(evalValue, exceptionInfoResponse.details.stackTrace);
        ToRelease <ICorDebugValue> evalValueOut;
        BOOL isNotNull = TRUE;

        ICorDebugValue *evalValueInner = pExceptionValue;
        while (isNotNull) {
            if ((res = m_evaluator.getObjectByFunction("get_InnerException", pThread, evalValueInner, &evalValueOut, defaultEvalFlags)) && FAILED(res))
                goto failed;

            string tmpstr;
            PrintValue(evalValueOut, tmpstr);

            if (tmpstr.compare("null") == 0)
                break;

            ToRelease<ICorDebugValue> pValueTmp;

            if ((res = DereferenceAndUnboxValue(evalValueOut, &pValueTmp, &isNotNull)) && FAILED(res))
                goto failed;

            hasInner = true;
            ExceptionDetails inner;
            PrintStringField(evalValueOut, message, inner.message);

            if ((res = m_evaluator.getObjectByFunction("get_StackTrace", pThread, evalValueOut, &pValueTmp, defaultEvalFlags)) && FAILED(res))
                goto failed;

            PrintValue(pValueTmp, inner.stackTrace);

            exceptionInfoResponse.details.innerException.push_back(inner);
            evalValueInner = evalValueOut;
        }
    }

    if (hasInner)
        exceptionInfoResponse.description += "\n Inner exception found, see $exception in variables window for more details.";

    m_evaluator.pop_eval_queue(); // CompleteException
    return S_OK;

failed:
    m_evaluator.pop_eval_queue(); // CompleteException
    IfFailRet(res);
    return res;
}

// MI
HRESULT ManagedDebugger::InsertExceptionBreakpoint(const ExceptionBreakMode &mode,
    const string &name, uint32_t &id)
{
    LogFuncEntry();
    return m_breakpoints.InsertExceptionBreakpoint(mode, name, id);
}

// MI
HRESULT ManagedDebugger::DeleteExceptionBreakpoint(const uint32_t id)
{
    LogFuncEntry();
    return m_breakpoints.DeleteExceptionBreakpoint(id);
}

// MI and VSCode
bool ManagedDebugger::MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const string &exceptionName,
    const ExceptionBreakCategory category)
{
    LogFuncEntry();
    return m_breakpoints.MatchExceptionBreakpoint(dwEventType, exceptionName, category);
}

HRESULT ManagedDebugger::SetEnableCustomNotification(BOOL fEnable)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(m_modules.GetModuleWithName("System.Private.CoreLib.dll", &pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    // in order to make code simple and clear, we don't check enclosing classes with recursion here
    // since we know behaviour for sure, just find "System.Diagnostics.Debugger" first
    mdTypeDef typeDefParent = mdTypeDefNil;
    static const WCHAR strParentTypeDef[] = W("System.Diagnostics.Debugger");
    IfFailRet(pMD->FindTypeDefByName(strParentTypeDef, mdTypeDefNil, &typeDefParent));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDef[] = W("CrossThreadDependencyNotification");
    IfFailRet(pMD->FindTypeDefByName(strTypeDef, typeDefParent, &typeDef));

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pModule->GetClassFromToken(typeDef, &pClass));

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pModule->GetProcess(&pProcess));

    ToRelease<ICorDebugProcess3> pProcess3;
    IfFailRet(pProcess->QueryInterface(IID_ICorDebugProcess3, (LPVOID*) &pProcess3));
    return pProcess3->SetEnableCustomNotification(pClass, fEnable);
}

HRESULT ManagedDebugger::SetBreakpoints(
    const std::string& filename,
    const std::vector<SourceBreakpoint> &srcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    return m_breakpoints.SetBreakpoints(m_pProcess, filename, srcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::SetFunctionBreakpoints(
    const std::vector<FunctionBreakpoint> &funcBreakpoints,
    std::vector<Breakpoint> &breakpoints)
{
    LogFuncEntry();

    return m_breakpoints.SetFunctionBreakpoints(m_pProcess, funcBreakpoints, breakpoints);
}

HRESULT ManagedDebugger::GetFrameLocation(ICorDebugFrame *pFrame, ThreadId threadId, FrameLevel level, StackFrame &stackFrame)
{
    HRESULT Status;

    stackFrame = StackFrame(threadId, level, "");

    ULONG32 ilOffset;
    Modules::SequencePoint sp;

    if (SUCCEEDED(m_modules.GetFrameILAndSequencePoint(pFrame, ilOffset, sp)))
    {
        stackFrame.source = Source(sp.document);
        stackFrame.line = sp.startLine;
        stackFrame.column = sp.startColumn;
        stackFrame.endLine = sp.endLine;
        stackFrame.endColumn = sp.endColumn;
    }

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 nOffset = 0;
    ToRelease<ICorDebugNativeFrame> pNativeFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &pNativeFrame));
    IfFailRet(pNativeFrame->GetIP(&nOffset));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    IfFailRet(Modules::GetModuleId(pModule, stackFrame.moduleId));

    stackFrame.clrAddr.methodToken = methodToken;
    stackFrame.clrAddr.ilOffset = ilOffset;
    stackFrame.clrAddr.nativeOffset = nOffset;

    stackFrame.addr = GetFrameAddr(pFrame);

    TypePrinter::GetMethodName(pFrame, stackFrame.name);

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT ManagedDebugger::GetStackTrace(ICorDebugThread *pThread, FrameLevel startFrame, unsigned maxFrames, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    LogFuncEntry();

    HRESULT Status;

    DWORD tid = 0;
    pThread->GetID(&tid);
    ThreadId threadId{tid};

    int currentFrame = -1;

    IfFailRet(WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < int(startFrame))
            return S_OK;
        if (maxFrames != 0 && currentFrame >= int(startFrame) + int(maxFrames))
            return S_OK;

        switch(frameType)
        {
            case FrameUnknown:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, "?");
                stackFrames.back().addr = GetFrameAddr(pFrame);
                break;
            case FrameNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, pNative->symbol);
                stackFrames.back().addr = pNative->addr;
                stackFrames.back().source = Source(pNative->file);
                stackFrames.back().line = pNative->linenum;
                break;
            case FrameCLRNative:
                stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, "[Native Frame]");
                stackFrames.back().addr = GetFrameAddr(pFrame);
                break;
            case FrameCLRInternal:
                {
                    ToRelease<ICorDebugInternalFrame> pInternalFrame;
                    IfFailRet(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame));
                    CorDebugInternalFrameType corFrameType;
                    IfFailRet(pInternalFrame->GetFrameType(&corFrameType));
                    std::string name = "[";
                    name += GetInternalTypeName(corFrameType);
                    name += "]";
                    stackFrames.emplace_back(threadId, FrameLevel{currentFrame}, name);
                    stackFrames.back().addr = GetFrameAddr(pFrame);
                }
                break;
            case FrameCLRManaged:
                {
                    StackFrame stackFrame;
                    GetFrameLocation(pFrame, threadId, FrameLevel{currentFrame}, stackFrame);
                    stackFrames.push_back(stackFrame);
                }
                break;
        }
        return S_OK;
    }));

    totalFrames = currentFrame + 1;

    return S_OK;
}

int ManagedDebugger::GetNamedVariables(uint32_t variablesReference)
{
    LogFuncEntry();

    return m_variables.GetNamedVariables(variablesReference);
}

HRESULT ManagedDebugger::GetVariables(
    uint32_t variablesReference,
    VariablesFilter filter,
    int start,
    int count,
    std::vector<Variable> &variables)
{
    LogFuncEntry();

    return m_variables.GetVariables(m_pProcess, variablesReference, filter, start, count, variables);
}

HRESULT ManagedDebugger::GetScopes(FrameId frameId, std::vector<Scope> &scopes)
{
    LogFuncEntry();

    return m_variables.GetScopes(m_pProcess, frameId, scopes);
}

HRESULT ManagedDebugger::Evaluate(FrameId frameId, const std::string &expression, Variable &variable, std::string &output)
{
    LogFuncEntry();

    return m_variables.Evaluate(m_pProcess, frameId, expression, variable, output);
}

HRESULT ManagedDebugger::SetVariable(const std::string &name, const std::string &value, uint32_t ref, std::string &output)
{
    return m_variables.SetVariable(m_pProcess, name, value, ref, output);
}

HRESULT ManagedDebugger::SetVariableByExpression(
    FrameId frameId,
    const Variable &variable,
    const std::string &value,
    std::string &output)
{
    HRESULT Status;
    ToRelease<ICorDebugValue> pResultValue;

    IfFailRet(m_variables.GetValueByExpression(m_pProcess, frameId, variable, &pResultValue));
    return m_variables.SetVariable(m_pProcess, pResultValue, value, frameId, output);
}


void ManagedDebugger::FindFileNames(string_view pattern, unsigned limit, SearchCallback cb)
{
    m_modules.FindFileNames(pattern, limit, cb);
}

void ManagedDebugger::FindFunctions(string_view pattern, unsigned limit, SearchCallback cb)
{
    m_modules.FindFunctions(pattern, limit, cb);
}

void ManagedDebugger::FindVariables(ThreadId thread, FrameLevel framelevel, string_view pattern, unsigned limit, SearchCallback cb)
{
    StackFrame frame{thread, framelevel, ""};
    std::vector<Scope> scopes;
    std::vector<Variable> variables;
    HRESULT status = GetScopes(frame.id, scopes);
    if (FAILED(status))
    {
        LOGW("GetScopes failed: %s", errormessage(status));
        return;
    }

    if (scopes.empty() || scopes[0].variablesReference == 0)
    {
        LOGW("no variables in visible scopes");
        return;
    }

    status = GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables);
    if (FAILED(status))
    {
        LOGW("GetVariables failed: %s", errormessage(status));
        return;
    }

    for (const Variable& var : variables)
    {
        LOGD("var: '%s'", var.name.c_str());

        if (limit == 0)
            break;

        auto pos = var.name.find(pattern.data(), 0, pattern.size());
        if (pos != std::string::npos && (pos == 0 || var.name[pos-1] == '.'))
        {
            limit--;
            cb(var.name.c_str());
        }
    }
}


void ManagedDebugger::InputCallback(IORedirectHelper::StreamType type, span<char> text)
{
    m_protocol->EmitOutputEvent(type == IOSystem::Stderr ? OutputStdErr : OutputStdOut, {text.begin(), text.size()});
}


// Check if breakpoint is part of async stepping routine and do next action for async stepping if need.
// [in] pAppDomain - object that represents the application domain that contains the breakpoint.
// [in] pThread - object that represents the thread that contains the breakpoint.
// [in] pBreakpoint - object that represents the breakpoint.
bool ManagedDebugger::HitAsyncStepBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    ToRelease<ICorDebugFrame> pFrame;
    mdMethodDef methodToken;
    if (FAILED(pThread->GetActiveFrame(&pFrame)) ||
        pFrame == nullptr ||
        FAILED(pFrame->GetFunctionToken(&methodToken)))
    {
        LOGE("Failed receive function token for async step");
        return false;
    }
    CORDB_ADDRESS modAddress;
    ToRelease<ICorDebugFunction> pFunc;
    ToRelease<ICorDebugModule> pModule;
    if (FAILED(pFrame->GetFunction(&pFunc)) ||
        FAILED(pFunc->GetModule(&pModule)) ||
        FAILED(pModule->GetBaseAddress(&modAddress)))
    {
        LOGE("Failed receive module address for async step");
        return false;
    }

    const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);

    if (!m_asyncStep)
    {
        // Care special case here, when we step-out from async method with await blocks
        // and NotifyDebuggerOfWaitCompletion magic happens with breakpoint in this method.
        // Note, if we hit NotifyDebuggerOfWaitCompletion breakpoint, it's our no matter which thread.

        if (modAddress != modAddressNotifyDebuggerOfWaitCompletion ||
            methodToken != methodTokenNotifyDebuggerOfWaitCompletion)
            return false;

        modAddressNotifyDebuggerOfWaitCompletion = 0;
        methodTokenNotifyDebuggerOfWaitCompletion = mdMethodDefNil;
        pBreakpoint->Activate(FALSE);
        // Note, notification flag will be reseted automatically in NotifyDebuggerOfWaitCompletion() method,
        // no need call SetNotificationForWaitCompletion() with FALSE arg (at least, mono acts in the same way).

        // Update stepping request to new thread/frame_count that we are continuing on
        // so continuing with normal step-out works as expected.
        SetupSimpleStep(pThread, Debugger::STEP_OUT);
        pAppDomain->Continue(0);
        return true;
    }

    if (modAddress != m_asyncStep->m_Breakpoint->modAddress ||
        methodToken != m_asyncStep->m_Breakpoint->methodToken)
    {
        // Async step was breaked by another breakpoint, remove async step related breakpoint.
        // Same behavior as MS vsdbg have for stepping interrupted by breakpoint.
        m_asyncStep.reset(nullptr);
        return false;
    }

    ToRelease<ICorDebugILFrame> pILFrame;
    ULONG32 ipOffset;
    CorDebugMappingResult mappingResult;
    if (FAILED(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame)) ||
        FAILED(pILFrame->GetIP(&ipOffset, &mappingResult)))
    {
        LOGE("Failed receive current IP offset for async step");
        return false;
    }

    if (ipOffset != m_asyncStep->m_Breakpoint->ilOffset)
    {
        // Async step was breaked by another breakpoint, remove async step related breakpoint.
        // Same behavior as MS vsdbg have for stepping interrupted by breakpoint.
        m_asyncStep.reset(nullptr);
        return false;
    }

    if (m_asyncStep->m_stepStatus == ManagedDebugger::asyncStepStatus::yield_offset_breakpoint)
    {
        // Note, in case of first breakpoint for async step, we must have same thread.
        if (m_asyncStep->m_threadId != getThreadId(pThread))
        {
            // Parallel thread execution, skip it and continue async step routine.
            pAppDomain->Continue(0);
            return true;
        }

        ToRelease<ICorDebugProcess> pProcess;
        if (SUCCEEDED(pThread->GetProcess(&pProcess)))
            DisableAllSteppers(pProcess);

        m_asyncStep->m_stepStatus = ManagedDebugger::asyncStepStatus::resume_offset_breakpoint;

        ToRelease<ICorDebugCode> pCode;
        ToRelease<ICorDebugFunctionBreakpoint> pBreakpoint;
        if (FAILED(pFunc->GetILCode(&pCode)) ||
            FAILED(pCode->CreateBreakpoint(m_asyncStep->m_resume_offset, &pBreakpoint)) ||
            FAILED(pBreakpoint->Activate(TRUE)))
        {
            LOGE("Could not setup second breakpoint (resume_offset) for await block");
            return false;
        }

        m_asyncStep->m_Breakpoint->iCorBreakpoint->Activate(FALSE);
        m_asyncStep->m_Breakpoint->iCorBreakpoint = pBreakpoint.Detach();
        m_asyncStep->m_Breakpoint->ilOffset = m_asyncStep->m_resume_offset;

        ToRelease<ICorDebugAppDomain> callbackAppDomain(pAppDomain);
        pAppDomain->AddRef();
        ToRelease<ICorDebugThread> callbackThread(pThread);
        pThread->AddRef();
        ToRelease<ICorDebugFrame> callbackFrame(pFrame.GetPtr());
        pFrame->AddRef();

        // Switch to separate thread, since we must return runtime's callback call before evaluation start.
        std::thread([this](
            ICorDebugAppDomain *pAppDomain,
            ICorDebugThread *pThread,
            ICorDebugFrame *pFrame)
        {
            const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);

            ToRelease<ICorDebugValue> pValueRef;
            if (FAILED(GetAsyncIdReference(pThread, pFrame, m_evaluator, &pValueRef)) ||
                FAILED(pValueRef->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &m_asyncStep->pValueAsyncIdRef)))
                LOGE("Could not setup reference pValueAsyncIdRef for await block");

            pAppDomain->Continue(0);
        },
            std::move(callbackAppDomain),
            std::move(callbackThread),
            std::move(callbackFrame)
        ).detach();
    }
    else
    {
        // For second breakpoint we could have 3 cases:
        // 1. We still have initial thread, so, no need spend time and check asyncId.
        // 2. We have another thread with same asyncId - same execution of async method.
        // 3. We have another thread with different asyncId - parallel execution of async method.
        if (m_asyncStep->m_threadId == getThreadId(pThread))
        {
            SetupSimpleStep(pThread, m_asyncStep->m_initialStepType);
            m_asyncStep.reset(nullptr);
            pAppDomain->Continue(0);
            return true;
        }

        ToRelease<ICorDebugAppDomain> callbackAppDomain(pAppDomain);
        pAppDomain->AddRef();
        ToRelease<ICorDebugThread> callbackThread(pThread);
        pThread->AddRef();
        ToRelease<ICorDebugFrame> callbackFrame(pFrame.GetPtr());
        pFrame->AddRef();

        // Switch to separate thread, since we must return runtime's callback call before evaluation start.
        std::thread([this](
            ICorDebugAppDomain *pAppDomain,
            ICorDebugThread *pThread,
            ICorDebugFrame *pFrame)
        {
            const std::lock_guard<std::mutex> lock_async(m_asyncStepMutex);

            ToRelease<ICorDebugValue> pValueRef;
            GetAsyncIdReference(pThread, pFrame, m_evaluator, &pValueRef);

            CORDB_ADDRESS currentAsyncId = 0;
            ToRelease<ICorDebugValue> pValue;
            BOOL isNull = FALSE;
            if (SUCCEEDED(DereferenceAndUnboxValue(pValueRef, &pValue, &isNull)) && !isNull)
                pValue->GetAddress(&currentAsyncId);
            else
                LOGE("Could not calculate current async ID for await block");

            CORDB_ADDRESS prevAsyncId = 0;
            ToRelease<ICorDebugValue> pDereferencedValue;
            ToRelease<ICorDebugValue> pValueAsyncId;
            if (m_asyncStep->pValueAsyncIdRef && // Note, we could fail with pValueAsyncIdRef on previous breakpoint by some reason.
                SUCCEEDED(m_asyncStep->pValueAsyncIdRef->Dereference(&pDereferencedValue)) &&
                SUCCEEDED(DereferenceAndUnboxValue(pDereferencedValue, &pValueAsyncId, &isNull)) && !isNull)
                pValueAsyncId->GetAddress(&prevAsyncId);
            else
                LOGE("Could not calculate previous async ID for await block");

            // Note, 'currentAsyncId' and 'prevAsyncId' is 64 bit addresses, in our case can't be 0.
            // If we can't detect proper thread - continue stepping for this thread.
            if (currentAsyncId == prevAsyncId || currentAsyncId == 0 || prevAsyncId == 0)
            {
                SetupSimpleStep(pThread, m_asyncStep->m_initialStepType);
                m_asyncStep.reset(nullptr);
            }

            pAppDomain->Continue(0);
        },
            std::move(callbackAppDomain),
            std::move(callbackThread),
            std::move(callbackFrame)
        ).detach();
    }

    return true;
}

} // namespace netcoredbg
