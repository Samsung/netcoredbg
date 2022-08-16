// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/attributes.h"

#include <functional>
#include <algorithm>

#include "metadata/typeprinter.h"


namespace netcoredbg
{

static bool ForEachAttribute(IMetaDataImport *pMD, mdToken tok, std::function<HRESULT(const std::string &AttrName)> cb)
{
    bool found = false;
    ULONG numAttributes = 0;
    HCORENUM fEnum = NULL;
    mdCustomAttribute attr;
    while(SUCCEEDED(pMD->EnumCustomAttributes(&fEnum, tok, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
    {
        std::string mdName;
        mdToken ptkObj = mdTokenNil;
        mdToken ptkType = mdTokenNil;
        if (FAILED(pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, nullptr, nullptr)) ||
            FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, nullptr)))
            continue;

        found = cb(mdName);
        if (found)
            break;
    }
    pMD->CloseEnum(fEnum);
    return found;
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName)
{
    return ForEachAttribute(pMD, tok, [&attrName](const std::string &AttrName) -> bool
    {
        return AttrName == attrName;
    });
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames)
{
    return ForEachAttribute(pMD, tok, [&attrNames](const std::string &AttrName) -> bool
    {
        return std::find(attrNames.begin(), attrNames.end(), AttrName) != attrNames.end();
    });
}

} // namespace netcoredbg
