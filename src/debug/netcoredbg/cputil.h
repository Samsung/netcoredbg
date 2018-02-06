// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>
#include <locale>
#include <codecvt>

static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;

static inline std::string to_utf8(const WCHAR *wstr, int len = -1)
{
    if (len == -1)
        return convert.to_bytes(wstr);
    if (len == 0)
        return std::string();
    return convert.to_bytes(wstr, wstr + len);
}
