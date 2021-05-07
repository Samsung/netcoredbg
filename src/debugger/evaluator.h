// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_set>
#include "protocols/protocol.h"
#include "torelease.h"

namespace netcoredbg
{

class Modules;
class EvalWaiter;

class Evaluator
{
public:
    typedef std::function<HRESULT(mdMethodDef,ICorDebugModule*,ICorDebugType*,ICorDebugValue*,bool,const std::string&)> WalkMembersCallback;
    typedef std::function<HRESULT(ICorDebugValue*,const std::string&)> WalkStackVarsCallback;

    enum ValueKind
    {
        ValueIsScope,
        ValueIsClass,
        ValueIsVariable
    };

    Evaluator(std::shared_ptr<Modules> &sharedModules,
              std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
        m_sharedModules(sharedModules),
        m_sharedEvalWaiter(sharedEvalWaiter)
    {}

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

    HRESULT EvalExpr(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        const std::string &expression,
        ICorDebugValue **ppResult,
        int evalFlags);

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
        FrameLevel frameLevel,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        WalkStackVarsCallback cb);

    HRESULT CreateString(
        ICorDebugThread *pThread,
        const std::string &value,
        ICorDebugValue **ppNewString);

    void Cleanup();

private:

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalWaiter> m_sharedEvalWaiter;
    ToRelease<ICorDebugFunction> m_pSuppressFinalize;

    HRESULT FollowNested(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        const std::string &methodClass,
        const std::vector<std::string> &parts,
        ICorDebugValue **ppResult,
        int evalFlags);
    HRESULT FollowFields(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pValue,
        ValueKind valueKind,
        const std::vector<std::string> &parts,
        int nextPart,
        ICorDebugValue **ppResult,
        int evalFlags);
    HRESULT GetFieldOrPropertyWithName(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pInputValue,
        ValueKind valueKind,
        const std::string &name,
        ICorDebugValue **ppResultValue,
        int evalFlags);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugType *pTypeCast,
        WalkMembersCallback cb);

    HRESULT HandleSpecialLocalVar(
        const std::string &localName,
        ICorDebugValue *pLocalValue,
        std::unordered_set<std::string> &locals,
        WalkStackVarsCallback cb);

    HRESULT HandleSpecialThisParam(
        ICorDebugValue *pThisValue,
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

};

} // namespace netcoredbg
