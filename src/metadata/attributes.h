// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <vector>
#include <string>

namespace netcoredbg
{

struct DebuggerAttribute
{
    static const char NonUserCode[];
    static const char StepThrough[];
    static const char Hidden[];
};

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName);
bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames);

} // namespace netcoredbg
