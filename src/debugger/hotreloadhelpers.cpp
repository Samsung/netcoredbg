// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <list>
#include <vector>
#include <sstream>

#include "debugger/hotreloadhelpers.h"
#include "debugger/evaluator.h"
#include "debugger/evalhelpers.h"
#include "metadata/modules.h"
#include "utils/torelease.h"

namespace netcoredbg
{
namespace HotReloadHelpers
{

#ifdef NCDB_DOTNET_STARTUP_HOOK
static std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}
#endif // NCDB_DOTNET_STARTUP_HOOK

// Get array of System.Type objects, that we will use as argument for `ClearCache()` and `UpdateApplication()` methods.
static HRESULT GetMetadataUpdateTypes(ICorDebugThread *pThread, EvalHelpers *pEvalHelpers, const std::string &updatedDLL,
                                      const std::unordered_set<mdTypeDef> &updatedTypeTokens, ICorDebugValue **ppResultValue)
{
#ifdef NCDB_DOTNET_STARTUP_HOOK
    HRESULT Status;

    ToRelease<ICorDebugValue> iCorArg1Value;
    pEvalHelpers->CreateString(pThread, updatedDLL, &iCorArg1Value);

    // For second argument we use string with class tokens separated by `;` symbol.
    // In this way we avoid creation bunch of `System.UInt32` objects (one for each token) and managed array (that will hold all this objects).
    std::ostringstream ss;
    for (const auto &typeToken : updatedTypeTokens)
    {
        if (!ss.str().empty())
            ss << ";";
        ss << typeToken;
    }
    ToRelease<ICorDebugValue> iCorArg2Value;
    pEvalHelpers->CreateString(pThread, ss.str(), &iCorArg2Value);

    static const std::string assemblyName = GetFileName(NCDB_DOTNET_STARTUP_HOOK);
    static const WCHAR className[] = W("StartupHook");
    static const WCHAR methodName[] = W("ncdbGetMetadataUpdateTypes");
    ToRelease<ICorDebugFunction> iCorFunc;
    IfFailRet(pEvalHelpers->FindMethodInModule(assemblyName, className, methodName, &iCorFunc));
    ICorDebugValue *ppArgsValue[] = {iCorArg1Value, iCorArg2Value};
    IfFailRet(pEvalHelpers->EvalFunction(pThread, iCorFunc.GetPtr(), nullptr, 0, ppArgsValue, 2, ppResultValue, defaultEvalFlags));

    return S_OK;
#else // NCDB_DOTNET_STARTUP_HOOK
    return E_NOTIMPL;
#endif // NCDB_DOTNET_STARTUP_HOOK
}

// Call all ClearCache() and UpdateApplication() methods from UpdateHandlerTypes.
HRESULT UpdateApplication(ICorDebugThread *pThread, Modules *pModules, Evaluator *pEvaluator, EvalHelpers *pEvalHelpers,
                          const std::string &updatedDLL, const std::unordered_set<mdTypeDef> &updatedTypeTokens)
{
    HRESULT Status;
    std::vector<ToRelease<ICorDebugType>> modulesUpdateHandlerTypes;
    pModules->CopyModulesUpdateHandlerTypes(modulesUpdateHandlerTypes);

    std::list<ToRelease<ICorDebugFunction>> listClearCache;
    std::list<ToRelease<ICorDebugFunction>> listUpdateApplication;
    std::vector<Evaluator::ArgElementType> emptyVector;
    for (auto &updateHandlerType : modulesUpdateHandlerTypes)
    {
        ToRelease<ICorDebugType> iCorResultType;
        pEvaluator->WalkMethods(updateHandlerType.GetPtr(), &iCorResultType, emptyVector, [&](
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

    ToRelease<ICorDebugValue> iCorArgValue;
    if (FAILED(GetMetadataUpdateTypes(pThread, pEvalHelpers, updatedDLL, updatedTypeTokens, &iCorArgValue)))
    {
        // Create `null`.
        ToRelease<ICorDebugEval> iCorEval;
        IfFailRet(pThread->CreateEval(&iCorEval));
        IfFailRet(iCorEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, &iCorArgValue));
    }

    for (auto &func : listClearCache)
    {
        IfFailRet(pEvalHelpers->EvalFunction(pThread, func.GetPtr(), nullptr, 0, iCorArgValue.GetRef(), 1, nullptr, defaultEvalFlags));
    }
    for (auto &func : listUpdateApplication)
    {
        IfFailRet(pEvalHelpers->EvalFunction(pThread, func.GetPtr(), nullptr, 0, iCorArgValue.GetRef(), 1, nullptr, defaultEvalFlags));
    }

    return S_OK;
}

} // namespace HotReloadHelpers

} // namespace netcoredbg
