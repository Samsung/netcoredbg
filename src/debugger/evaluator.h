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

    struct SetterData
    {
        ToRelease<ICorDebugValue> thisValue;
        ToRelease<ICorDebugType> propertyType;
        ToRelease<ICorDebugFunction> setterFunction;

        SetterData(ICorDebugValue *pValue, ICorDebugType *pType, ICorDebugFunction *pFunction)
        {
            Set(pValue, pType, pFunction);
        };

        SetterData(SetterData &setterData)
        {
            Set(setterData.thisValue.GetPtr(), setterData.propertyType.GetPtr(), setterData.setterFunction.GetPtr());
        };

        void Set(ICorDebugValue *pValue, ICorDebugType *pType, ICorDebugFunction *pFunction)
        {
            if (pValue)
                pValue->AddRef();
            thisValue = pValue;

            if (pType)
                pType->AddRef();
            propertyType = pType;

            if (pFunction)
                pFunction->AddRef();
            setterFunction = pFunction;
        }
    };

    typedef std::function<HRESULT(ICorDebugValue**,int)> GetValueCallback;
    typedef std::function<HRESULT(ICorDebugType*,bool,const std::string&,GetValueCallback,SetterData*)> WalkMembersCallback;
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
        SetterData *inputSetterData,
        std::vector<std::string> &identifiers,
        ICorDebugValue **ppResultValue,
        std::unique_ptr<SetterData> *resultSetterData,
        ICorDebugType **ppResultType,
        int evalFlags);

    HRESULT WalkMembers(
        ICorDebugValue *pValue,
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        bool provideSetterData,
        WalkMembersCallback cb);

    HRESULT WalkStackVars(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        WalkStackVarsCallback cb);

    HRESULT Evaluator::GetMethodClass(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        std::string &methodClass,
        bool &thisParam);

    HRESULT GetElement(ICorDebugValue *pInputValue, std::vector<ULONG32> &indexes, ICorDebugValue **ppResultValue);
    HRESULT WalkMethods(ICorDebugType *pInputType, WalkMethodsCallback cb);
    HRESULT WalkMethods(ICorDebugValue *pInputTypeValue, WalkMethodsCallback cb);

    HRESULT SetValue(ICorDebugThread *pThread, FrameLevel frameLevel, ICorDebugValue *pValue, SetterData *setterData,
                     const std::string &value, int evalFlags, std::string &output);

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
        std::unique_ptr<SetterData> *resultSetterData,
        int evalFlags);

    HRESULT FollowFields(
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugValue *pValue,
        ValueKind valueKind,
        std::vector<std::string> &identifiers,
        int nextIdentifier,
        ICorDebugValue **ppResult,
        std::unique_ptr<SetterData> *resultSetterData,
        int evalFlags);

    HRESULT WalkMembers(
        ICorDebugValue *pInputValue,
        ICorDebugThread *pThread,
        FrameLevel frameLevel,
        ICorDebugType *pTypeCast,
        bool provideSetterData,
        WalkMembersCallback cb);
};

} // namespace netcoredbg
