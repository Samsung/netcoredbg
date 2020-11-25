// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "utils/utf.h"

#include <codecvt>
#include <locale>

namespace netcoredbg
{

#ifdef _MSC_VER

static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t> convert;

std::string to_utf8(const wchar_t *wstr)
{
    return convert.to_bytes(wstr);
}

#else

static std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert;

std::string to_utf8(const char16_t *wstr)
{
    return convert.to_bytes(wstr);
}

#endif

std::string to_utf8(char16_t wch)
{
    return convert.to_bytes(wch);
}

#ifdef _MSC_VER
std::wstring
#else
std::u16string
#endif
to_utf16(const std::string &utf8)
{
    return convert.from_bytes(utf8);
}

std::vector<std::string> split_on_tokens(const std::string &str, const char delim)
{
    std::vector<std::string> res;
    size_t pos = 0, prev = 0;

    while (true)
    {
        pos = str.find(delim, prev);
        if (pos == std::string::npos)
        {
            res.push_back(std::string(str, prev));
            break;
        }

        res.push_back(std::string(str, prev, pos - prev));
        prev = pos + 1;
    }

    return res;
}

} // namespace netcoredbg
