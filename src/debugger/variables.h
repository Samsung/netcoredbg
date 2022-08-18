// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include <unordered_map>
#include "interfaces/types.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class Evaluator;
class EvalHelpers;
class EvalWaiter;
class EvalStackMachine;

class Variables
{
public:

    Variables(std::shared_ptr<EvalHelpers> &sharedEvalHelpers,
              std::shared_ptr<Evaluator> &sharedEvaluator,
              std::shared_ptr<EvalStackMachine> &sharedEvalStackMachine) :
        m_sharedEvalHelpers(sharedEvalHelpers),
        m_sharedEvaluator(sharedEvaluator),
        m_sharedEvalStackMachine(sharedEvalStackMachine)
    {}

    int GetNamedVariables(uint32_t variablesReference);

    HRESULT GetVariables(
        ICorDebugProcess *pProcess,
        uint32_t variablesReference,
        VariablesFilter filter,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT SetVariable(
        ICorDebugProcess *pProcess,
        const std::string &name,
        const std::string &value,
        uint32_t ref,
        std::string &output);

    HRESULT SetExpression(
        ICorDebugProcess *pProcess,
        FrameId frameId,
        const std::string &expression,
        int evalFlags,
        const std::string &value,
        std::string &output);

    HRESULT GetScopes(
        ICorDebugProcess *pProcess,
        FrameId frameId,
        std::vector<Scope> &scopes);

    HRESULT Evaluate(
        ICorDebugProcess *pProcess,
        FrameId frameId,
        const std::string &expression,
        Variable &variable,
        std::string &output);

    HRESULT GetExceptionVariable(
        FrameId frameId,
        ICorDebugThread *pThread,
        Variable &variable);

    void Clear()
    {
        m_referencesMutex.lock();
        m_references.clear();
        m_referencesMutex.unlock();
    }

private:

    enum ValueKind
    {
        ValueIsScope,
        ValueIsClass,
        ValueIsVariable
    };

    struct VariableReference
    {
        uint32_t variablesReference; // key
        int namedVariables;
        int indexedVariables;
        int evalFlags;

        std::string evaluateName;

        ValueKind valueKind;
        ToRelease<ICorDebugValue> iCorValue;
        FrameId frameId;

        VariableReference(const Variable &variable, FrameId frameId, ICorDebugValue *pValue, ValueKind valueKind) :
            variablesReference(variable.variablesReference),
            namedVariables(variable.namedVariables),
            indexedVariables(variable.indexedVariables),
            evalFlags(variable.evalFlags),
            evaluateName(variable.evaluateName),
            valueKind(valueKind),
            iCorValue(pValue),
            frameId(frameId)
        {}

        VariableReference(uint32_t variablesReference, FrameId frameId, int namedVariables) :
            variablesReference(variablesReference),
            namedVariables(namedVariables),
            indexedVariables(0),
            evalFlags(0), // unused in this case, not involved into GetScopes routine
            valueKind(ValueIsScope),
            iCorValue(nullptr),
            frameId(frameId)
        {}

        bool IsScope() const { return valueKind == ValueIsScope; }

        VariableReference(VariableReference &&that) = default;
        VariableReference(const VariableReference &that) = delete;
    };

    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    std::shared_ptr<EvalStackMachine> m_sharedEvalStackMachine;

    std::recursive_mutex m_referencesMutex;
    std::unordered_map<uint32_t, VariableReference> m_references;

    HRESULT AddVariableReference(Variable &variable, FrameId frameId, ICorDebugValue *pValue, ValueKind valueKind);

    HRESULT GetStackVariables(
        FrameId frameId,
        ICorDebugThread *pThread,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT GetChildren(
        VariableReference &ref,
        ICorDebugThread *pThread,
        int start,
        int count,
        std::vector<Variable> &variables);

    HRESULT SetStackVariable(
        VariableReference &ref,
        ICorDebugThread *pThread,
        const std::string &name,
        const std::string &value,
        std::string &output);

    HRESULT SetChild(
        VariableReference &ref,
        ICorDebugThread *pThread,
        const std::string &name,
        const std::string &value,
        std::string &output);

};

} // namespace netcoredbg
