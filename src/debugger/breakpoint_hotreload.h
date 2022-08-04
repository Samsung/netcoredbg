// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include <memory>
#include <unordered_set>
#include <string>
#include "utils/torelease.h"

namespace netcoredbg
{

class Modules;
class Evaluator;
class EvalHelpers;

class HotReloadBreakpoint
{
public:

    HotReloadBreakpoint(std::shared_ptr<Modules> &sharedModules, std::shared_ptr<Evaluator> &sharedEvaluator, std::shared_ptr<EvalHelpers> &sharedEvalHelpers) :
        m_sharedModules(sharedModules),
        m_sharedEvaluator(sharedEvaluator),
        m_sharedEvalHelpers(sharedEvalHelpers)
    {}

    HRESULT SetHotReloadBreakpoint(const std::string &updatedDLL, const std::unordered_set<mdTypeDef> &updatedTypeTokens);
    void Delete();

    // Important! Must provide succeeded return code:
    // S_OK - internal HotReload breakpoint hit
    // S_FALSE - not internal HotReload breakpoint hit
    HRESULT CheckApplicationReload(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);
    void CheckApplicationReload(ICorDebugThread *pThread);

    HRESULT ManagedCallbackLoadModuleAll(ICorDebugModule *pModule);

private:

    std::mutex m_reloadMutex;
    std::shared_ptr<Modules> m_sharedModules;
    std::shared_ptr<Evaluator> m_sharedEvaluator;
    std::shared_ptr<EvalHelpers> m_sharedEvalHelpers;
    ToRelease<ICorDebugFunction> m_iCorFunc;
    ToRelease<ICorDebugFunctionBreakpoint> m_iCorFuncBreakpoint;
    std::string m_updatedDLL;
    std::unordered_set<mdTypeDef> m_updatedTypeTokens;

    void Clear();
};

} // namespace netcoredbg
