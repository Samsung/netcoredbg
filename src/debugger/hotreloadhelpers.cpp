// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <list>
#include <vector>

#include "debugger/hotreloadhelpers.h"
#include "debugger/evaluator.h"
#include "debugger/evalhelpers.h"
#include "metadata/modules.h"
#include "utils/torelease.h"

namespace netcoredbg
{
namespace HotReloadHelpers
{

// Call all ClearCache() and UpdateApplication() methods from UpdateHandlerTypes.
HRESULT UpdateApplication(ICorDebugThread *pThread, Modules *pModules, Evaluator *pEvaluator, EvalHelpers *pEvalHelpers)
{
    HRESULT Status;
    std::vector<ToRelease<ICorDebugType>> modulesUpdateHandlerTypes;
    pModules->CopyModulesUpdateHandlerTypes(modulesUpdateHandlerTypes);

    std::list<ToRelease<ICorDebugFunction>> listClearCache;
    std::list<ToRelease<ICorDebugFunction>> listUpdateApplication;
    for (auto &updateHandlerType : modulesUpdateHandlerTypes)
    {
        pEvaluator->WalkMethods(updateHandlerType.GetPtr(), [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType &methodRet,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            
            if (!is_static ||
                methodRet.corType != ELEMENT_TYPE_VOID ||
                methodArgs.size() != 1 ||
                methodArgs[0].corType != ELEMENT_TYPE_SZARRAY ||
                methodArgs[0].typeName != "System.Type[]")
                return S_OK;

            if (methodName == "ClearCache")
            {
                listClearCache.emplace_back();
                IfFailRet(getFunction(&listClearCache.back()));
            }
            else if (methodName == "UpdateApplication")
            {
                listUpdateApplication.emplace_back();
                IfFailRet(getFunction(&listUpdateApplication.back()));
            }

            return S_OK;
        });
    }

    // TODO send real type array (for all changed types) instead of null
    ToRelease<ICorDebugEval> iCorEval;
    IfFailRet(pThread->CreateEval(&iCorEval));
    ToRelease<ICorDebugValue> iCorNullValue;
    IfFailRet(iCorEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, &iCorNullValue));

    for (auto &func : listClearCache)
    {
        IfFailRet(pEvalHelpers->EvalFunction(pThread, func.GetPtr(), nullptr, 0, iCorNullValue.GetRef(), 1, nullptr, defaultEvalFlags));
    }
    for (auto &func : listUpdateApplication)
    {
        IfFailRet(pEvalHelpers->EvalFunction(pThread, func.GetPtr(), nullptr, 0, iCorNullValue.GetRef(), 1, nullptr, defaultEvalFlags));
    }

    return S_OK;
}

} // namespace HotReloadHelpers

} // namespace netcoredbg
