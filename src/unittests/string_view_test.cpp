// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>

#include <limits.h>

#ifndef TEST_NATIVE_STRING_VIEW
#include "utils/string_view.h"
using ::netcoredbg::Utility::string_view;

#else
#include <string_view>
using std::string_view;
#endif

TEST_CASE("StringView::Assign")
{
	string_view z;
	CHECK(z.empty());
	CHECK(z.size() == 0);

	const char *str = "abcdefg";
	string_view n(str);
	CHECK(!(n.empty()));
	CHECK(n.size() == 7);
	CHECK(n.data() == "abcdefg");

	string_view m("zxcvb");
	CHECK(!(m.empty()));
	CHECK(m.size() == 5);
	CHECK(m.data() == "zxcvb");

	string_view k(n);
	CHECK(!(k.empty()));
	CHECK(k.size() == 7);
	CHECK(k.data() == "abcdefg");

	z = n;
	CHECK(!(z.empty()));
	CHECK(z.size() == 7);
	CHECK(z.data() == "abcdefg");
	CHECK(z.length() == 7);

	z.swap(m);
	CHECK(z.data() == "zxcvb");
	CHECK(z.size() == 5);
	CHECK(m.data() == "abcdefg");
	CHECK(m.size() == 7);

        const char str1[11] = "0\00023456789";  // check for null inside string
        string_view q(str1);
        CHECK(q.size() == 10);

        char str2[20] = "0123456789";  // check length correctness
        string_view v(str2);
        CHECK(v.size() == 10);

	CHECK(z.max_size() >= SHRT_MAX);
}

TEST_CASE("StringView::Access")
{
	const char *str = "0123456789";
	string_view s(str);
	CHECK(s[2] == '2');
	CHECK(s.at(3) == '3');
	CHECK(s.data() == str);
	CHECK(s.front() == '0');
	CHECK(s.back() == '9');
}

TEST_CASE("StringView::Compare")
{
	string_view s("0123456789");

	CHECK(s.compare("0123456789") == 0);
	CHECK(s.compare("01234567891") < 0);
	CHECK(s.compare("012345678") > 0);
	CHECK(s.compare("012346") < 0);
	CHECK(s.compare("012344") > 0);

	CHECK(s.compare(2, 3, "234") == 0);
	CHECK(s.compare(2, 3, "234xxxx", 3) == 0);

	CHECK(string_view("0123456789") == s);
	CHECK(!(string_view("0123456789") != s));
	CHECK(!(string_view("xxx") == s));
	CHECK(string_view("xxx") != s);

	CHECK(string_view("abc") < string_view("acc"));
	CHECK(string_view("abd") > string_view("abc"));
	CHECK(string_view("abc") <= string_view("abc"));
	CHECK(string_view("abc") >= string_view("abc"));
}

TEST_CASE("StringView::Change")
{
	string_view s("0123456789");

	s.remove_prefix(2);
	CHECK(s == string_view("23456789"));

	s.remove_suffix(3);
	CHECK(s == string_view("23456"));
}

TEST_CASE("StringView::Splice")
{
	string_view s("0123456789");

	string_view v(s.substr(0, string_view::npos));
	CHECK(v == string_view("0123456789"));

	v = s.substr(2);
	CHECK(v == string_view("23456789"));

	v = s.substr(3, 2);
	CHECK(v == string_view("34"));
}

TEST_CASE("StringView::Copy")
{
	string_view s("0123456789");
	char buf[128];
	memset(buf, 'x', sizeof(buf));

	s.copy(&buf[50], 0);
	CHECK(buf[50] == 'x');

	s.copy(&buf[50], 5);
	CHECK(buf[55] == 'x');
	buf[55] = 0;
	CHECK(!strcmp(&buf[50], "01234"));

	memset(buf, 'x', sizeof(buf));
	s.copy(&buf[50], SHRT_MAX, 2);
	CHECK(buf[58] == 'x');
	buf[58] = 0;
	CHECK(!strcmp(&buf[50], "23456789"));

}

TEST_CASE("StringView::Find")
{
	string_view s("01234567890123456789");

	CHECK(s.find('5') == 5);
	CHECK(s.find('x') == string_view::npos);

	CHECK(s.find("345") == 3);
	CHECK(s.find("345", 4) == 13);
	CHECK(s.find("345", 1) == 3);
	CHECK(s.find("346", 0, 2) == 3);
}

TEST_CASE("StringView::RevFind")
{
	string_view s("01234567890123456789");

	CHECK(s.rfind('5') == 15);
	CHECK(s.rfind('x') == string_view::npos);

	CHECK(s.rfind("345") == 13);
	CHECK(s.rfind("345", 10) == 3);		// XXX `pos' determines right boundary!
	CHECK(s.rfind("345", 2) == string_view::npos);
	CHECK(s.rfind("346", 10, 2) == 3);
}

TEST_CASE("StringView::FindFirstOf")
{
	string_view s("01234567890123456789");

	CHECK(s.find_first_of('5') == 5);
	CHECK(s.find_first_of('x') == string_view::npos);

	CHECK(s.find_first_of("543") == 3);
	CHECK(s.find_first_of("543", 4) == 4);
	CHECK(s.find_first_of("543", 1) == 3);
	CHECK(s.find_first_of("543", 0, 2) == 4);
}

TEST_CASE("StringView::FindLastOf")
{
	string_view s("01234567890123456789");

	CHECK(s.find_last_of('5') == 15);
	CHECK(s.find_last_of('x') == string_view::npos);

	CHECK(s.find_last_of("543") == 15);			// XXX `pos' is the right boundary!
	CHECK(s.find_last_of("543", 2) == string_view::npos);
	CHECK(s.find_last_of("543", 10) == 5);
	CHECK(s.find_last_of("543", 15, 2) == 15);
}

TEST_CASE("StringView::FindFirstNotOf")
{
	string_view s("01234567890123456789");

	CHECK(s.find_first_not_of('0') == 1);
	CHECK(s.find_first_not_of("0123456789") == string_view::npos);

	CHECK(s.find_first_not_of("01234") == 5);
	CHECK(s.find_first_not_of("234", 2) == 5);
	CHECK(s.find_first_not_of("2340123456789", 2, 3) == 5);
}

TEST_CASE("StringView::FindLastNotOf")
{
	string_view s("01234567890123456789");

	CHECK(s.find_last_not_of('9') == 18);
	CHECK(s.find_last_not_of("0123456789") == string_view::npos);

	CHECK(s.find_last_not_of("6789") == 15);		// XXX `pos' is the right boundary!
	CHECK(s.find_last_not_of("6789", 9) == 5);
	CHECK(s.find_last_not_of("67890123456789",9, 4) == 5);
}

TEST_CASE("StringView::starts_with")
{
    string_view s("0123456789");
    CHECK(s.starts_with(string_view{"0123"}));
    CHECK(!s.starts_with(string_view{"123"}));
    CHECK(s.starts_with('0'));
    CHECK(!s.starts_with('1'));
    CHECK(s.starts_with("0123"));
    CHECK(!s.starts_with("123"));
    char v[] = "0123\0xxx";
    char q[] = "1234\0xxx";
    CHECK(s.starts_with(v));
    CHECK(!s.starts_with(q));
}

TEST_CASE("StringView::ends_with")
{
    string_view s("0123456789");
    CHECK(s.ends_with(string_view{"6789"}));
    CHECK(!s.ends_with(string_view{"678"}));
    CHECK(s.ends_with('9'));
    CHECK(!s.ends_with('8'));
    CHECK(s.ends_with("6789"));
    CHECK(!s.ends_with("678"));
    char v[] = "6789\0xxx";
    char q[] = "678\0xxx";
    CHECK(s.ends_with(v));
    CHECK(!s.ends_with(q));
}

TEST_CASE("StringView::contains")
{
    string_view s("0123456789");
    CHECK(s.contains(string_view{"3456"}));
    CHECK(s.contains(string_view{"0123"}));
    CHECK(s.contains(string_view{"6789"}));
    CHECK(!s.contains(string_view{"xxx"}));
    CHECK(s.contains('9'));
    CHECK(s.contains('0'));
    CHECK(s.contains('5'));
    CHECK(!s.contains('x'));
    CHECK(s.contains("0123"));
    CHECK(s.contains("6789"));
    CHECK(s.contains("3456"));
    CHECK(!s.contains("xxx"));
    char v1[] = "6789\0xxx";
    char v2[] = "0123\0xxx";
    char v3[] = "3456\0xxx";
    char q[] = "yyy\0003456";
    CHECK(s.contains(v1));
    CHECK(s.contains(v2));
    CHECK(s.contains(v3));
    CHECK(!s.contains(q));
}

