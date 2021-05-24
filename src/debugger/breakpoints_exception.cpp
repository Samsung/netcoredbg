// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_exception.h"
#include "debugger/evaluator.h"
#include "debugger/variables.h"
#include "debugger/valueprint.h"
#include "debugger/threads.h"
#include "metadata/typeprinter.h"
#include "utils/utf.h"

namespace netcoredbg
{

HRESULT ExceptionBreakpoints::ExceptionBreakpointStorage::Insert(uint32_t id, const ExceptionBreakMode &mode, const std::string &name)
{
    HRESULT Status = S_OK;
    // vsdbg each time creates a new exception breakpoint id.
    // But, for "*" name, the last `id' silently are deleted by vsdbg.
    if (name.compare("*") == 0)
    {
        if (bp.current_asterix_id != 0)
        {
            // Silent remove for global filter
            Status = Delete(bp.current_asterix_id);
        }
        bp.current_asterix_id = id;
    }

    bp.exceptionBreakpoints.insert(std::make_pair(name, mode));
    bp.table[id] = name;

    return Status;
}

HRESULT ExceptionBreakpoints::ExceptionBreakpointStorage::Delete(uint32_t id)
{
    const auto it = bp.table.find(id);
    if (it == bp.table.end())
    {
        return E_FAIL;
    }
    const std::string name = it->second;
    if (name.compare("*") == 0)
    {
        bp.current_asterix_id = 0;
    }
    bp.exceptionBreakpoints.erase(name);
    bp.table.erase(id);

    return S_OK;
}

bool ExceptionBreakpoints::ExceptionBreakpointStorage::Match(int dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category) const
{
    // INFO: #pragma once - its a reason for this constants
    const int FIRST_CHANCE = 1;
    const int USER_FIRST_CHANCE = 2;
    const int CATCH_HANDLER_FOUND = 3;
    const int UNHANDLED = 4;

    bool unsupported = (dwEventType == FIRST_CHANCE || dwEventType == USER_FIRST_CHANCE);
    if (unsupported)
        return false;

    // Try to match exactly by name after check global name "*"
    // ExceptionBreakMode can be specialized by explicit filter.
    ExceptionBreakMode mode;
    GetExceptionBreakMode(mode, "*");
    GetExceptionBreakMode(mode, exceptionName);
    if (category == ExceptionBreakCategory::ANY || category == mode.category)
    {
        if (dwEventType == CATCH_HANDLER_FOUND)
        {
            if (mode.UserUnhandled())
            {
                // Expected user-applications exceptions from throw(), but get
                // explicit/implicit exception from `System.' clases.
                const std::string SystemPrefix = "System.";
                if (exceptionName.compare(0, SystemPrefix.size(), SystemPrefix) != 0)
                    return true;
            }
            if (mode.Throw())
                return true;
        }
        if (dwEventType == UNHANDLED)
        {
            if (mode.Unhandled())
                return true;
        }
    }

    return false;
}

HRESULT ExceptionBreakpoints::ExceptionBreakpointStorage::GetExceptionBreakMode(ExceptionBreakMode &out, const std::string &name) const
{
    auto p = bp.exceptionBreakpoints.equal_range(name);
    if (p.first == bp.exceptionBreakpoints.end())
    {
        return E_FAIL;
    }

    out.category = p.first->second.category;
    out.flags |= p.first->second.flags;
    ++p.first;
    while (p.first != p.second)
    {
        if (out.category == ExceptionBreakCategory::ANY ||
            out.category == p.first->second.category)
        {
            out.flags |= p.first->second.flags;
        }
        ++p.first;
    }

    return S_OK;
}

HRESULT ExceptionBreakpoints::InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_exceptionBreakpointsMutex);
    return m_exceptionBreakpoints.Insert(id, mode, name);
}

HRESULT ExceptionBreakpoints::DeleteExceptionBreakpoint(uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_exceptionBreakpointsMutex);
    return m_exceptionBreakpoints.Delete(id);
}

HRESULT ExceptionBreakpoints::GetExceptionBreakMode(ExceptionBreakMode &mode, const std::string &name)
{
    std::lock_guard<std::mutex> lock(m_exceptionBreakpointsMutex);
    return m_exceptionBreakpoints.GetExceptionBreakMode(mode, name);
}

bool ExceptionBreakpoints::MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &name, const ExceptionBreakCategory category)
{
    std::lock_guard<std::mutex> lock(m_exceptionBreakpointsMutex);
    return m_exceptionBreakpoints.Match(dwEventType, name, category);
}

HRESULT ExceptionBreakpoints::GetExceptionInfoResponse(ICorDebugProcess *pProcess, ThreadId threadId, ExceptionInfoResponse &exceptionInfoResponse)
{
    // Are needed to move next line to Exception() callback?
    assert(int(threadId) != -1);

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

        if ((res = GetExceptionBreakMode(mode, "*")) && FAILED(res))
            goto failed;

        exceptionInfoResponse.breakMode = mode;
    }

    if ((res = pProcess->GetThread(int(threadId), &pThread)) && FAILED(res))
        goto failed;

    if ((res = pThread->GetCurrentException(&pExceptionValue)) && FAILED(res)) {
        LOGE("GetCurrentException() failed, %0x", res);
        goto failed;
    }

    PrintStringField(pExceptionValue, message, exceptionInfoResponse.description);

    if ((res = m_sharedVariables->GetExceptionVariable(frameId, pThread, varException)) && FAILED(res))
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

    if (FAILED(m_sharedEvaluator->getObjectByFunction("get_StackTrace", pThread, pExceptionValue, &evalValue, defaultEvalFlags))) {
        exceptionInfoResponse.details.stackTrace = "<undefined>"; // Evaluating problem entire object
    }
    else {
        PrintValue(evalValue, exceptionInfoResponse.details.stackTrace);
        ToRelease <ICorDebugValue> evalValueOut;
        BOOL isNotNull = TRUE;

        ICorDebugValue *evalValueInner = pExceptionValue;
        while (isNotNull) {
            if ((res = m_sharedEvaluator->getObjectByFunction("get_InnerException", pThread, evalValueInner, &evalValueOut, defaultEvalFlags)) && FAILED(res))
                goto failed;

            std::string tmpstr;
            PrintValue(evalValueOut, tmpstr);

            if (tmpstr.compare("null") == 0)
                break;

            ToRelease<ICorDebugValue> pValueTmp;

            if ((res = DereferenceAndUnboxValue(evalValueOut, &pValueTmp, &isNotNull)) && FAILED(res))
                goto failed;

            hasInner = true;
            ExceptionDetails inner;
            PrintStringField(evalValueOut, message, inner.message);

            if ((res = m_sharedEvaluator->getObjectByFunction("get_StackTrace", pThread, evalValueOut, &pValueTmp, defaultEvalFlags)) && FAILED(res))
                goto failed;

            PrintValue(pValueTmp, inner.stackTrace);

            exceptionInfoResponse.details.innerException.push_back(inner);
            evalValueInner = evalValueOut;
        }
    }

    if (hasInner)
        exceptionInfoResponse.description += "\n Inner exception found, see $exception in variables window for more details.";

    return S_OK;

failed:
    IfFailRet(res);
    return res;
}

static HRESULT GetExceptionInfo(ICorDebugThread *pThread, std::string &excType, std::string &excModule)
{
    HRESULT Status;

    ToRelease<ICorDebugValue> pExceptionValue;
    IfFailRet(pThread->GetCurrentException(&pExceptionValue));

    TypePrinter::GetTypeOfValue(pExceptionValue, excType);

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    WCHAR mdName[mdNameLen];
    ULONG nameLen;
    IfFailRet(pMDImport->GetScopeProps(mdName, _countof(mdName), &nameLen, nullptr));
    excModule = to_utf8(mdName);
    return S_OK;
}

HRESULT ExceptionBreakpoints::ManagedCallbackException(ICorDebugThread *pThread, CorDebugExceptionCallbackType dwEventType, StoppedEvent &event, std::string &textOutput)
{
    // INFO: Exception event callbacks produce Stop process and managed threads in coreCLR
    // After emit Stop event from debugger coreclr by command handler send a ExceptionInfo request.
    // For answer on ExceptionInfo are needed long FuncEval() with asynchronous EvalComplete event.
    // Of course evaluations is not atomic for coreCLR. Before EvalComplete we can get a new
    // ExceptionEvent if we allow to running of current thread.
    //
    // Current implementation stops all threads while EvalComplete waiting. But, unfortunately,
    // it's not helps in any cases. Exceptions can be throws in the same time from some threads.
    // And in this case threads thread suspend is not guaranteed, becase thread can stay in
    // "GC unsafe mode" or "Optimized code". And also, same time exceptions puts in priority queue event,
    // and all next events one by one will transport each exception.
    // For "GC unsafe mode" or "Optimized code" we cannot invoke CreateEval() function.

    HRESULT Status;
    std::string excType, excModule;
    if (FAILED(Status = GetExceptionInfo(pThread, excType, excModule)))
    {
        excType = "unknown exception";
        excModule = "unknown module";
        LOGI("Can't get exception info: %s", errormessage(Status));
    }

    ExceptionBreakMode mode;
    GetExceptionBreakMode(mode, "*");
    bool unhandled = (dwEventType == DEBUG_EXCEPTION_UNHANDLED && mode.Unhandled());
    bool not_matched = !(unhandled || MatchExceptionBreakpoint(dwEventType, excType, ExceptionBreakCategory::CLR));

    if (not_matched)
    {
        textOutput = "Exception thrown: '" + excType + "' in " + excModule + "\n";
        return S_FALSE;
    }

    std::string details;
    if (unhandled)
    {
        details = "An unhandled exception of type '" + excType + "' occurred in " + excModule;
        std::lock_guard<std::mutex> lock(m_lastUnhandledExceptionThreadIdsMutex);
        ThreadId threadId(getThreadId(pThread));
        m_lastUnhandledExceptionThreadIds.insert(threadId);
    }
    else
    {
        details = "Exception thrown: '" + excType + "' in " + excModule;
    }

    std::string message;
    WCHAR fieldName[] = W("_message\0");
    ToRelease<ICorDebugValue> pExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&pExceptionValue)))
        PrintStringField(pExceptionValue, fieldName, message);

    event.text = excType;
    event.description = message.empty() ? details : message;


    return S_OK;
}

} // namespace netcoredbg
