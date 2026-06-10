// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <vector>
#include <string>

namespace netcoredbg
{

struct DebuggerAttribute
{
    static const char NonUserCode[];
    static const char StepThrough[];
    static const char Hidden[];
    static const char Browsable[];
    static const char Display[];
};

// Walk over all custom attributes of `tok` and provide the attribute name plus the raw attribute
// value blob (ECMA-335 II.23.3) to the callback. Stop the walk and return `true` on first callback
// `true` return, return `false` in case no callback returned `true` during the walk.
bool ForEachAttribute(IMetaDataImport *pMD, mdToken tok,
                      std::function<bool(const std::string &attrName, const void *pBlob, ULONG cbBlobSize)> cb);

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName);
bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames);
bool HasDebuggerBrowsableNeverAttribute(IMetaDataImport *pMD, mdProperty propertyDef);
// Find `attrName` attribute on `tok` and decode the first fixed string argument of its
// constructor from the attribute value blob. Return `false` in case attribute not found
// or its first fixed argument is not a string.
bool GetAttributeFixedStringArgument(IMetaDataImport *pMD, mdToken tok, const char *attrName, std::string &value);

} // namespace netcoredbg
