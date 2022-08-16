// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <list>
#include <vector>
#include "utils/torelease.h"

namespace netcoredbg
{

class ModulesAppUpdate
{
public:

    HRESULT AddUpdateHandlerTypesForModule(ICorDebugModule *pModule, IMetaDataImport *pMD);
    void CopyModulesUpdateHandlerTypes(std::vector<ToRelease<ICorDebugType>> &modulesUpdateHandlerTypes);

    void Clear()
    {
        m_modulesUpdateHandlerTypes.clear();
    }

private:

    // Must care about topological sort during ClearCache() and UpdateApplication() methods calls at Hot Reload.
    std::list<ToRelease<ICorDebugType>> m_modulesUpdateHandlerTypes;

};

} // namespace netcoredbg
