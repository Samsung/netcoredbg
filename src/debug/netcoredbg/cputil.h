// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>
#include <locale>
#include <codecvt>

static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;

static inline std::string to_utf8(const char16_t *wstr)
{
    return convert.to_bytes(wstr);
}

static inline std::string to_utf8(char16_t wch)
{
    return convert.to_bytes(wch);
}
