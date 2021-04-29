// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "utils/utf.h"

#include <codecvt>
#include <locale>

namespace netcoredbg
{

static std::wstring_convert<std::codecvt_utf8_utf16<WCHAR>,WCHAR> convert;

std::string to_utf8(const WCHAR *wstr)
{
    return convert.to_bytes(wstr);
}

std::string to_utf8(WCHAR wch)
{
    return convert.to_bytes(wch);
}

WSTRING to_utf16(const std::string &utf8)
{
    return convert.from_bytes(utf8);
}

} // namespace netcoredbg
