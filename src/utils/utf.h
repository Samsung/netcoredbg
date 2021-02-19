// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "string_view.h"
#include <string>
#include <vector>

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <corerror.h>
#pragma GCC diagnostic pop

namespace netcoredbg
{
using Utility::string_view;

#ifdef _MSC_VER  // Visual Studio compiler
std::string to_utf8(const wchar_t *wstr);
std::wstring to_utf16(string_view utf8);

#else
std::string to_utf8(const char16_t *wstr);
std::u16string to_utf16(string_view utf8);
#endif

std::string to_utf8(char16_t wch);

template <typename CharT, size_t Size>
bool starts_with(const CharT *left, const CharT (&right)[Size])
{
    return std::char_traits<CharT>::compare(left, right, Size-1) == 0;
}

template <typename CharT, size_t Size>
bool str_equal(const CharT *left, const CharT (&right)[Size])
{
    return std::char_traits<CharT>::compare(left, right, Size) == 0;
}

// TODO move this function definition to separate header file.
const char* errormessage(HRESULT hr);

} // namespace netcoredbg
