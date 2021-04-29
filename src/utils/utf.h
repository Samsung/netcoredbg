// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>
#include <vector>

#ifdef _MSC_VER
#include <wtypes.h>
#endif

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <corerror.h>
#pragma GCC diagnostic pop

namespace netcoredbg
{

#ifdef _MSC_VER
typedef std::wstring WSTRING;
#else
typedef std::u16string WSTRING;
#endif

std::string to_utf8(const WCHAR *wstr);
WSTRING to_utf16(const std::string &utf8);
std::string to_utf8(WCHAR wch);

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
