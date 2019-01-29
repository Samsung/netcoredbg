// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <string>
#include <vector>

#ifdef _MSC_VER
std::string to_utf8(const wchar_t *wstr);
std::wstring to_utf16(const std::string &utf8);
#else
std::string to_utf8(const char16_t *wstr);
std::u16string to_utf16(const std::string &utf8);
#endif

std::string to_utf8(char16_t wch);
std::vector<std::string> split_on_tokens(const std::string &str, const char delim);
