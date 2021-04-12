// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <future>
#include <unordered_set>

#include "debugger/manageddebugger.h"

namespace netcoredbg
{

enum ValueKind
{
    ValueIsScope,
    ValueIsClass,
    ValueIsVariable
};

class Evaluator
{
public:

    typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
    typedef std::function<HRESULT(ICorDebugILFrame*,ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

    Modules &m_modules;

    std::mutex m_evalQueueMutex;
    std::list<ThreadId> m_evalQueue;

    bool is_empty_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        return m_evalQueue.empty();
    }

    void push_eval_queue(ThreadId tid) {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        m_evalQueue.push_back(tid);
    }

    ThreadId front_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        if (!m_evalQueue.empty())
            return m_evalQueue.front();
        return ThreadId{};
    }

    void pop_eval_queue() {
        std::lock_guard<std::mutex> lock(m_evalQueueMutex);
        if (!m_evalQueue.empty())
            m_evalQueue.pop_front();
    }

private:

    ToRelease<ICorDebugFunction> m_pSuppressFinalize;

    struct evalResult_t {
        evalResult_t() = delete;
        evalResult_t(ICorDebugEval *pEval_, const std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > &promiseValue_) = delete;
        evalResult_t(const evalResult_t &B) = delete;
        evalResult_t& operator = (const evalResult_t &B) = delete;
        evalResult_t& operator = (evalResult_t &&B) = delete;

        evalResult_t(ICorDebugEval *pEval_, std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > &&promiseValue_) :
            pEval(pEval_),
            promiseValue(std::move(promiseValue_))
        {}
        evalResult_t(evalResult_t &&B) :
            pEval(B.pEval),
            promiseValue(std::move(B.promiseValue))
        {}

        ~evalResult_t() = default;

        ICorDebugEval *pEval;
        std::promise< std::unique_ptr<ToRelease<ICorDebugValue>> > promiseValue;
    };

    std::mutex m_evalMutex;
    std::unordered_map< DWORD, evalResult_t > m_evalResults;

    HRESULT FollowNested(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         const std::string &methodClass,
                         const std::vector<std::string> &parts,
                         ICorDebugValue **ppResult,
                         int evalFlags);
    HRESULT FollowFields(ICorDebugThread *pThread,
                         ICorDebugILFrame *pILFrame,
                         ICorDebugValue *pValue,
                         ValueKind valueKind,
                         const std::vector<std::string> &parts,
                         int nextPart,
                         ICorDebugValue **ppResult,
                         int evalFlags);
    HRESULT GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                       ICorDebugILFrame *pILFrame,
                                       ICorDebugValue *pInputValue,
                                       ValueKind valueKind,
                                       const std::string &name,
                                       ICorDebugValue **ppResultValue,
                                       int evalFlags);

    HRESULT WaitEvalResult(ICorDebugThread *pThread,
                           ICorDebugEval *pEval,
                           ICorDebugValue **ppEvalResult);

    std::future< std::unique_ptr<ToRelease<ICorDebugValue>> > RunEval(
        ICorDebugThread *pThread,
        ICorDebugEval *pEval);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        ICorDebugType *pTypeCast,
        WalkMembersCallback cb);

    HRESULT HandleSpecialLocalVar(
        const std::string &localName,
        ICorDebugValue *pLocalValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT HandleSpecialThisParam(
        ICorDebugValue *pThisValue,
        ICorDebugILFrame *pILFrame,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT GetLiteralValue(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugModule *pModule,
        PCCOR_SIGNATURE pSignatureBlob,
        ULONG sigBlobLength,
        UVCP_CONSTANT pRawValue,
        ULONG rawValueLength,
        ICorDebugValue **ppLiteralValue);

    HRESULT FindType(
        const std::vector<std::string> &parts,
        int &nextPart,
        ICorDebugThread *pThread,
        ICorDebugModule *pModule,
        ICorDebugType **ppType,
        ICorDebugModule **ppModule = nullptr);

    HRESULT ResolveParameters(
        const std::vector<std::string> &params,
        ICorDebugThread *pThread,
        std::vector< ToRelease<ICorDebugType> > &types);

    static HRESULT FindFunction(
        ICorDebugModule *pModule,
        const WCHAR *typeName,
        const WCHAR *methodName,
        ICorDebugFunction **ppFunction);

public:

    Evaluator(Modules &modules) : m_modules(modules) {}

    HRESULT CreatTypeObjectStaticConstructor(
        ICorDebugThread *pThread,
        ICorDebugType *pType,
        ICorDebugValue **ppTypeObjectResult = nullptr,
        bool DetectStaticMembers = true);

    HRESULT EvalFunction(
        ICorDebugThread *pThread,
        ICorDebugFunction *pFunc,
        ICorDebugType **ppArgsType,
        ULONG32 ArgsTypeCount,
        ICorDebugValue **ppArgsValue,
        ULONG32 ArgsValueCount,
        ICorDebugValue **ppEvalResult,
        int evalFlags);

    HRESULT EvalExpr(ICorDebugThread *pThread,
                     ICorDebugFrame *pFrame,
                     const std::string &expression,
                     ICorDebugValue **ppResult,
                     int evalFlags);

    bool IsEvalRunning();
    ICorDebugEval *FindEvalForThread(ICorDebugThread *pThread);

    // Should be called by ICorDebugManagedCallback
    void NotifyEvalComplete(ICorDebugThread *pThread, ICorDebugEval *pEval);

    HRESULT getObjectByFunction(
        const std::string &func,
        ICorDebugThread *pThread,
        ICorDebugValue *pInValue,
        ICorDebugValue **ppOutValue,
        int evalFlags);

    HRESULT GetType(
        const std::string &typeName,
        ICorDebugThread *pThread,
        ICorDebugType **ppType);

    HRESULT WalkMembers(
        ICorDebugValue *pValue,
        ICorDebugThread *pThread,
        ICorDebugILFrame *pILFrame,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(ICorDebugFrame *pFrame, WalkStackVarsCallback cb);

    HRESULT CreateString(
        ICorDebugThread *pThread,
        const std::string &value,
        ICorDebugValue **ppNewString);

    void Cleanup();
};

} // namespace netcoredbg
