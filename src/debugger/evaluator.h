// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_set>
#include <list>
#include <vector>
#include <mutex>
#include "interfaces/types.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;
class EvalHelpers;
class EvalStackMachine;

class Evaluator
{
public:

    struct ArgElementType
    {
        CorElementType corType;
        std::string typeName;

        ArgElementType() :
            corType(ELEMENT_TYPE_END)
        {}
    };
    typedef ArgElementType ReturnElementType;

    typedef std::function<HRESULT(ICorDebugValue**,int)> GetValueCallback;
    typedef std::function<HRESULT(const std::string&,std::string&,int)> SetValueCallback;
    typedef std::function<HRESULT(ICorDebugType*,bool,const std::string&,GetValueCallback,SetValueCallback)> WalkMembersCallback;
    typedef std::function<HRESULT(const std::string&,GetValueCallback)> WalkStackVarsCallback;
    typedef std::function<HRESULT(ICorDebugFunction**)> GetFunctionCallback;
    typedef std::function<HRESULT(bool,const std::string&,ReturnElementType&,std::vector<ArgElementType>&,GetFunctionCallback)> WalkMethodsCallback;

    enum ValueKind
    {
        ValueIsScope,
        ValueIsClass,
        ValueIsVariable
    };

    Evaluator(std::shared_ptr<Modules> &sharedModules,
              std::shared_ptr<EvalHelpers> &sharedEvalHelpers,
              std::shared_ptr<EvalStackMachine> &sharedEvalStackMachine) :
        m_sharedModules(sharedModules),
        m_sharedEvalHelpers(sharedEvalHelpers),
        m_sharedEvalStackMachine(sharedEvalStackMachine)
    {}

    HRESULT ResolveIdentifiers(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pInputValue,
        std::vector<std::string> &identifiers,
        ICorDebugValue **ppResultValue,
        ICorDebugType **ppResultType,
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

    HRESULT GetElement(ICorDebugValue *pInputValue, std::vector<ULONG32> &indexes, ICorDebugValue **ppResultValue);
    HRESULT WalkMethods(ICorDebugType *pInputType, WalkMethodsCallback cb);
    HRESULT WalkMethods(ICorDebugValue *pInputTypeValue, WalkMethodsCallback cb);

private:

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    std::shared_ptr<EvalStackMachine> m_sharedEvalStackMachine;

    HRESULT FollowNestedFindValue(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        const std::string &methodClass,
        std::vector<std::string> &identifiers,
        ICorDebugValue **ppResult,
        int evalFlags);

    HRESULT FollowFields(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pValue,
        ValueKind valueKind,
        std::vector<std::string> &identifiers,
        int nextIdentifier,
        ICorDebugValue **ppResult,
        int evalFlags);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugType *pTypeCast,
        WalkMembersCallback cb);
};

} // namespace netcoredbg
