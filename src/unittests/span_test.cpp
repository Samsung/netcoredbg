// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include "span.h"

using ::netcoredbg::Utility::span;

static const char str[] = "test string";

TEST_CASE("default-constructor")
{
    span<char> s;
    CHECK(s.empty());
    CHECK(s.size() == 0);
    CHECK(s.begin() == s.end());
    const span<char>& cref(s);
    CHECK(cref.begin() == cref.end());
    CHECK(cref.begin() == s.end());
    CHECK(s.end() - s.begin() == 0);
}

TEST_CASE("construct-from-range")
{
    span<const char> s(&str[0], &str[sizeof(str)]);
    CHECK(!s.empty());
    CHECK(s.size() == sizeof(str));
    CHECK(s.size_bytes() == s.size());
    CHECK(s.data() == str);
}

TEST_CASE("construct-from-size")
{
    span<const char> s(str, sizeof(str));
    CHECK(!s.empty());
    CHECK(s.size() == sizeof(str));
    CHECK(s.size_bytes() == s.size());
    CHECK(s.data() == str);
}

TEST_CASE("index")
{
    char str[] = "12345";
    span<char> s(str, sizeof(str));
    CHECK(&s[0] == str);
    CHECK(&s.front() == &s[0]);
    CHECK(&s.back() == &s[sizeof(str)-1]);
    CHECK(&s[sizeof(str)-1] == &str[sizeof(str)-1]);
    s[0] = '0';
    CHECK(str[0] == '0');
}

TEST_CASE("subspan")
{
    span<const char> s(str, sizeof(str));
    CHECK(s.subspan(0).size() == s.size());
    CHECK(s.subspan(0).data() == s.data());
    CHECK(s.subspan(1).size() == s.size() - 1);
    CHECK(s.subspan(1).data() == &s[1]);
    CHECK(s.subspan(0, 1).size() == 1);
    CHECK(s.subspan(0, 1).data() == s.data());
    CHECK(s.subspan(0, 0).size() == 0);
}

TEST_CASE("iterators")
{
    span<const char> s(str, sizeof(str));
    CHECK(s.end() - s.begin() == sizeof(str));
    CHECK(&*s.begin() == str);
    CHECK(&*(s.begin()+1) == &str[1]);
    CHECK(&*(s.end()-1) == &str[sizeof(str)-1]);
}

TEST_CASE("non-char")
{
    static const int array[] = {1, 2, 3};
    span<const int> s(array, sizeof(array)/sizeof(array[0]));
    CHECK(s.size() == sizeof(array)/sizeof(array[0]));
    CHECK(s.size_bytes() == sizeof(array));
}

// TODO add test for operator=
