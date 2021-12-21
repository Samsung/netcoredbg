// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <future>
#include "utils/torelease.h"

namespace netcoredbg
{

class Threads;

class EvalWaiter
{
public:

    typedef std::function<HRESULT(ICorDebugEval*)> WaitEvalResultCallback;

    EvalWaiter(std::shared_ptr<Threads> &sharedThreads) : m_sharedThreads(sharedThreads), m_evalCanceled(false) {}

    bool IsEvalRunning();
    void CancelEvalRunning();
    ICorDebugEval *FindEvalForThread(ICorDebugThread *pThread);

    HRESULT WaitEvalResult(ICorDebugThread *pThread,
                           ICorDebugValue **ppEvalResult,
                           WaitEvalResultCallback cbSetupEval);

    // Should be called by ICorDebugManagedCallback.
    void NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval);
    HRESULT ManagedCallbackCustomNotification(ICorDebugThread *pThread);

private:

    std::shared_ptr<Threads> m_sharedThreads;
    bool m_evalCanceled;

    struct evalResultData_t
    {
        ToRelease<ICorDebugValue> iCorEval;
        HRESULT Status = E_FAIL;
    };

    struct evalResult_t {
        evalResult_t() = delete;
        evalResult_t(DWORD threadId_, ICorDebugEval *pEval_, const std::promise< std::unique_ptr<evalResultData_t> > &promiseValue_) = delete;
        evalResult_t(const evalResult_t &B) = delete;
        evalResult_t& operator = (const evalResult_t &B) = delete;
        evalResult_t& operator = (evalResult_t &&B) = delete;

        evalResult_t(DWORD threadId_, ICorDebugEval *pEval_, std::promise< std::unique_ptr<evalResultData_t> > &&promiseValue_) :
            threadId(threadId_),
            pEval(pEval_),
            promiseValue(std::move(promiseValue_))
        {}
        evalResult_t(evalResult_t &&B) :
            threadId(B.threadId),
            pEval(B.pEval),
            promiseValue(std::move(B.promiseValue))
        {}

        ~evalResult_t() = default;

        DWORD threadId;
        ICorDebugEval *pEval;
        std::promise< std::unique_ptr<evalResultData_t> > promiseValue;
    };

    std::mutex m_waitEvalResultMutex;
    std::mutex m_evalResultMutex;
    std::unique_ptr<evalResult_t> m_evalResult;

    std::future< std::unique_ptr<evalResultData_t> > RunEval(
        ICorDebugProcess *pProcess,
        ICorDebugThread *pThread,
        ICorDebugEval *pEval,
        WaitEvalResultCallback cbSetupEval);

};

} // namespace netcoredbg
