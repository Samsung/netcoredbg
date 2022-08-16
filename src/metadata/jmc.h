// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <unordered_set>

namespace netcoredbg
{

HRESULT DisableJMCByAttributes(ICorDebugModule *pModule);
HRESULT DisableJMCByAttributes(ICorDebugModule *pModule, const std::unordered_set<mdMethodDef> &methodTokens);

} // namespace netcoredbg
