// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_set>
#include <list>
#include <mutex>
#include "interfaces/types.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;
class EvalHelpers;

class Evaluator
{
public:
    typedef std::function<HRESULT(ICorDebugValue**,int)> GetValueCallback;
    typedef std::function<HRESULT(const std::string&,std::string&,int)> SetValueCallback;
    typedef std::function<HRESULT(ICorDebugType*,bool,const std::string&,GetValueCallback,SetValueCallback)> WalkMembersCallback;
    typedef std::function<HRESULT(const std::string&,GetValueCallback)> WalkStackVarsCallback;

    enum ValueKind
    {
        ValueIsScope,
        ValueIsClass,
        ValueIsVariable
    };

    Evaluator(std::shared_ptr<Modules> &sharedModules,
              std::shared_ptr<EvalHelpers> &sharedEvalHelpers) :
        m_sharedModules(sharedModules),
        m_sharedEvalHelpers(sharedEvalHelpers)
    {}

    HRESULT EvalExpr(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        std::vector<std::string> &identifiers,
        ICorDebugValue **ppResult,
        int evalFlags);

    HRESULT WalkMembers(
        ICorDebugValue *pValue,
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        WalkStackVarsCallback cb);

    HRESULT SetValue(
        ICorDebugValue *pValue,
        const std::string &value,
        ICorDebugThread *pThread,
        std::string &errorText);

    HRESULT FollowFields(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pValue,
        ValueKind valueKind,
        std::vector<std::string> &identifiers,
        int nextIdentifier,
        ICorDebugValue **ppResult,
        int evalFlags);

    HRESULT GetElement(ICorDebugValue *pInputValue, std::vector<ULONG32> &indexes, ICorDebugValue **ppResultValue);

private:

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;

    HRESULT FollowNested(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        const std::string &methodClass,
        std::vector<std::string> &identifiers,
        ICorDebugValue **ppResult,
        int evalFlags);

    HRESULT GetFieldOrPropertyByIdentifiers(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pInputValue,
        ValueKind valueKind,
        std::string &identifiers,
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

};

} // namespace netcoredbg
