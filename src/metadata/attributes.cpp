// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/attributes.h"

#include <functional>
#include <algorithm>

#include "metadata/typeprinter.h"


namespace netcoredbg
{

// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggerbrowsableattribute
// Determines if and how a member is displayed in the debugger variable windows.
const char DebuggerAttribute::Browsable[] = "System.Diagnostics.DebuggerBrowsableAttribute..ctor";

bool ForEachAttribute(IMetaDataImport *pMD, mdToken tok,
                      std::function<bool(const std::string &attrName, const void *pBlob, ULONG cbBlobSize)> cb)
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
        void const *pBlob = nullptr;
        ULONG cbBlobSize = 0;
        if (FAILED(pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, &pBlob, &cbBlobSize)) ||
            FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, nullptr)))
            continue;

        found = cb(mdName, pBlob, cbBlobSize);
        if (found)
            break;
    }
    pMD->CloseEnum(fEnum);
    return found;
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName)
{
    return ForEachAttribute(pMD, tok, [&attrName](const std::string &AttrName, const void*, ULONG) -> bool
    {
        return AttrName == attrName;
    });
}

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames)
{
    return ForEachAttribute(pMD, tok, [&attrNames](const std::string &AttrName, const void*, ULONG) -> bool
    {
        return std::find(attrNames.begin(), attrNames.end(), AttrName) != attrNames.end();
    });
}

bool HasDebuggerBrowsableNeverAttribute(IMetaDataImport *pMD, mdProperty propertyDef)
{
    // https://github.sec.samsung.net/dotnet/coreclr/blob/9df87a133b0f29f4932f38b7307c87d09ab80d5d/src/System.Private.CoreLib/shared/System/Diagnostics/DebuggerBrowsableAttribute.cs#L17
    // Since we check only first byte, no reason store it as int (default enum type in c#)
    enum DebuggerBrowsableState : char
    {
        Never = 0,
        Expanded = 1,
        Collapsed = 2,
        RootHidden = 3
    };

    return ForEachAttribute(pMD, propertyDef, [](const std::string &attrName, const void *pBlob, ULONG cbBlobSize) -> bool
    {
        // In case of DebuggerBrowsableAttribute blob is 8 bytes:
        // 2 bytes - blob prolog 0x0001
        // 4 bytes - data (DebuggerBrowsableAttribute::State), default enum type (int)
        // 2 bytes - alignment
        // We check only one byte (first data byte), no reason check 4 bytes in our case.
        return attrName == DebuggerAttribute::Browsable &&
               cbBlobSize > 2 &&
               ((char const *)pBlob)[2] == DebuggerBrowsableState::Never;
    });
}

} // namespace netcoredbg
