// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/attributes.h"

#include <functional>
#include <algorithm>
#include <cstdint>

#include "metadata/typeprinter.h"


namespace netcoredbg
{

// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggerbrowsableattribute
// Determines if and how a member is displayed in the debugger variable windows.
const char DebuggerAttribute::Browsable[] = "System.Diagnostics.DebuggerBrowsableAttribute..ctor";
// https://docs.microsoft.com/en-us/dotnet/api/system.diagnostics.debuggerdisplayattribute
// Determines how a class or field is displayed in the debugger variable windows.
const char DebuggerAttribute::Display[] = "System.Diagnostics.DebuggerDisplayAttribute..ctor";

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

// ECMA-335 II.23.2 Blobs and signatures, compressed unsigned integer decoding.
static bool ReadCompressedUInt(const BYTE *&pData, const BYTE *pEnd, uint32_t &value)
{
    if (pData >= pEnd)
        return false;

    BYTE b1 = *pData++;
    if ((b1 & 0x80) == 0)
    {
        value = b1;
        return true;
    }

    if ((b1 & 0xC0) == 0x80)
    {
        if (pEnd - pData < 1)
            return false;

        value = ((b1 & 0x3F) << 8) | *pData++;
        return true;
    }

    if ((b1 & 0xE0) == 0xC0)
    {
        if (pEnd - pData < 3)
            return false;

        value = ((b1 & 0x1F) << 24) |
                (static_cast<uint32_t>(pData[0]) << 16) |
                (static_cast<uint32_t>(pData[1]) << 8) |
                pData[2];
        pData += 3;
        return true;
    }

    return false;
}

// ECMA-335 II.23.3 Custom attributes: blob prolog 0x0001, then ctor fixed arguments,
// where a string argument is a SerString - packed length prefix plus UTF-8 bytes.
static bool DecodeCustomAttributeFixedStringArgument(const void *pBlob, ULONG cbBlobSize, std::string &value)
{
    const BYTE *pData = static_cast<const BYTE *>(pBlob);
    const BYTE *pEnd = pData + cbBlobSize;

    if (pEnd - pData < 2 || pData[0] != 0x01 || pData[1] != 0x00)
        return false;

    pData += 2;

    if (pData >= pEnd)
        return false;

    // SerString null marker, treat it as an empty fixed argument instead of reading past the blob.
    if (*pData == 0xFF)
    {
        value.clear();
        return true;
    }

    uint32_t stringLength = 0;
    if (!ReadCompressedUInt(pData, pEnd, stringLength) || static_cast<uint32_t>(pEnd - pData) < stringLength)
        return false;

    value.assign(reinterpret_cast<const char *>(pData), stringLength);
    return true;
}

bool GetAttributeFixedStringArgument(IMetaDataImport *pMD, mdToken tok, const char *attrName, std::string &value)
{
    return ForEachAttribute(pMD, tok, [&attrName, &value](const std::string &AttrName, const void *pBlob, ULONG cbBlobSize) -> bool
    {
        return AttrName == attrName && DecodeCustomAttributeFixedStringArgument(pBlob, cbBlobSize, value);
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
