// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <vector>
#include <string>

namespace netcoredbg
{

class Modules;

namespace EvalUtils
{
    HRESULT GetType(const std::string &typeName, ICorDebugThread *pThread, Modules *pModules, ICorDebugType **ppType);
    std::vector<std::string> ParseType(const std::string &expression, std::vector<int> &ranks);
    HRESULT FindType(const std::vector<std::string> &parts, int &nextPart, ICorDebugThread *pThread, Modules *pModules,
                     ICorDebugModule *pModule, ICorDebugType **ppType, ICorDebugModule **ppModule = nullptr);
}

} // namespace netcoredbg
