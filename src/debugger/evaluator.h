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

        ArgElementType(CorElementType t, std::string n)
        {
            corType = t;
            typeName = n;
        }

        bool isAlias (const CorElementType type1, const CorElementType type2, const std::string& name2);
        bool areEqual(const ArgElementType& arg);
        inline bool operator==(const ArgElementType& arg) { return areEqual(arg); }
        inline bool operator!=(const ArgElementType& arg) { return !areEqual(arg); }
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
    HRESULT WalkMethods(ICorDebugType *pInputType, std::vector<Evaluator::ArgElementType> &methodGenerics, WalkMethodsCallback cb);
    HRESULT WalkMethods(ICorDebugValue *pInputTypeValue, WalkMethodsCallback cb);
    HRESULT SetValue(ICorDebugThread *pThread, FrameLevel frameLevel, ICorDebugValue *pValue, SetterData *setterData,
                     const std::string &value, int evalFlags, std::string &output);

    ArgElementType GetElementTypeByTypeName(const std::string typeName);

private:

    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    std::shared_ptr<EvalStackMachine> m_sharedEvalStackMachine;

};

} // namespace netcoredbg
