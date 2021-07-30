// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <string>
#include <memory>
#include <vector>
#include <list>
#include "debugger/evaluationpart.h"
#include "interfaces/types.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class Evaluator;
class EvalHelpers;
class EvalWaiter;

struct EvalStackEntry
{
    std::vector<EvaluationPart> parts;
    ToRelease<ICorDebugValue> iCorValue;
};

struct EvalData
{
    ICorDebugThread *pThread;
    Evaluator *sharedEvaluator;
    EvalHelpers *pEvalHelpers;
    EvalWaiter *pEvalWaiter;
    ICorDebugClass *pDecimalClass;
    FrameLevel frameLevel;
    int evalFlags;

    EvalData() :
        pThread(nullptr), sharedEvaluator(nullptr), pEvalHelpers(nullptr), pEvalWaiter(nullptr), pDecimalClass(nullptr), evalFlags(defaultEvalFlags)
    {}
};

class EvalStackMachine
{
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    std::shared_ptr<EvalWaiter> m_sharedEvalWaiter;
    std::list<EvalStackEntry> m_evalStack;
    EvalData m_evalData;
    // In case of NumericLiteralExpression with Decimal, NewParameterizedObjectNoConstructor() are used.
    // Proper ICorDebugClass must be provided for Decimal (will be found during FindPredefinedTypes() call).
    ToRelease<ICorDebugClass> m_iCorDecimalClass;

public:

    EvalStackMachine(std::shared_ptr<Evaluator> &sharedEvaluator, std::shared_ptr<EvalHelpers> &sharedEvalHelpers, std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
        m_sharedEvaluator(sharedEvaluator),
        m_sharedEvalHelpers(sharedEvalHelpers),
        m_sharedEvalWaiter(sharedEvalWaiter)
    {
        m_evalData.sharedEvaluator = m_sharedEvaluator.get();
        m_evalData.pEvalHelpers = m_sharedEvalHelpers.get();
        m_evalData.pEvalWaiter = m_sharedEvalWaiter.get();
    }

    // Run stack machine for particular expression.
    HRESULT Run(ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags, const std::string &expression,
                ICorDebugValue **ppResultValue, std::string &output);

    // Find ICorDebugClass objects for all predefined types we need for stack machine during Private.CoreLib load.
    // See ManagedCallback::LoadModule().
    HRESULT FindPredefinedTypes(ICorDebugModule *pModule);

};

} // namespace netcoredbg
