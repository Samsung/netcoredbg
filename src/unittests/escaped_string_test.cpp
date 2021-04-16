// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include <string>
#include "string_view.h"
#include "utils/escaped_string.h"
#include "compile_test.h"

using namespace netcoredbg;
using string_view = Utility::string_view;

struct EscapeRules
{
    static const char forbidden_chars[];
    static const string_view subst_chars[];
    static const char constexpr escape_char = '\\';
};

const char EscapeRules::forbidden_chars[] = "\"\\\0\a\b\f\n\r\t\v";

const string_view EscapeRules::subst_chars[] {
    "\\\"", "\\\\", "\\0", "\\a", "\\b", "\\f", "\\n", "\\r", "\\t", "\\v"
};

using ES = EscapedString<EscapeRules>;

TEST_CASE("EscapedString")
{
    string_view s1 { "test12345" };
    string_view s2 { "test\n" };

    // assuming no copying
    CHECK(static_cast<string_view>(ES(s1)) == s1);
    const void *v1, *v2;
    v1 = static_cast<string_view>(ES(s1)).data(), v2  = s1.data();
    CHECK(v1 == v2);

    ES es1(s1);  // expecting copying
    CHECK(static_cast<string_view>(es1) == s1);
    v1 = static_cast<string_view>(es1).data(), v2  = s1.data();
    CHECK(v1 != v2);

    CHECK(static_cast<string_view>(ES("\n")) == "\\n");
    CHECK(static_cast<string_view>(ES("aaa\n")) == "aaa\\n");
    CHECK(static_cast<string_view>(ES("\naaa")) == "\\naaa");
    CHECK(static_cast<string_view>(ES("aaa\nbbb")) == string_view("aaa\\nbbb", 8));
    CHECK(static_cast<string_view>(ES(string_view("aaa\0bbb", 7))) == string_view("aaa\\0bbb", 8));

    std::string result;
    ES("aaa\nbbb")([&](string_view s) { result.append(s.begin(), s.end()); });
    CHECK(result == "aaa\\nbbb");
}

