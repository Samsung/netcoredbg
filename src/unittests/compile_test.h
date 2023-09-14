// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.
#pragma once

// This macros must be used in body of the test expression passed
// to COMPILE_TEST macros, in place of regular std::declval<T>().
#define TEST_DECLVAL(...) compile_test_imaginary_type<T, __VA_ARGS__>()

// Helper class for TEST_DECLVAL macros.
template <typename, typename ImaginaryType> ImaginaryType compile_test_imaginary_type();

// Helper types for COMPILE_TEST (see below).
template <typename...>  using _can_compile = void;
struct cant_compile { constexpr static bool value = false; };

// Helper macros for COMPILE_TEST (see below).
#if COMPILE_TEST_ASSERTS
#define CAN_COMPILE_ASSERT(val, name, decl) \
    static_assert(val, "this should not be compiled (" #name "): " #decl )
#else
#define CAN_COMPILE_ASSERT(name, decl, ...) static_assert(true, "")
#endif

// This macros checks, that VA_ARGS expression can be compiled:
// if COMPILE_TEST_ASSERTS set to non-zero value, compilation fails
// if COMPILE_TEST(name, expr) is failed. If COMPILE_TESTS_ASSERTS is not set,
// then COMPILE_TEST(name, expr) defines boolean variable `name' which is
// set to true, if expression `expr' was compiled successfully.
//
// Actually this macros not compiles given expression, but employs `decltype`
// operator to compute the type of the expression. So, if the expression can't
// be compiled, type deduction failed, and thanks to SFINAE principle
// compilation error is avoided. But please note, to avoid compilation error
// any use of `std::declval<T>()` function in test expression should be
// replaced with `TEST_DECLVAL(T)` macros.
//
// The main purpose of this macros, is to detect compilation error at
// compile time and just record the result, but not stop the compilation,
// and report errors later, in runtime. There is two reasons for this:
// 1) to allow few tests failed, but still performing the testing
//    (not to stop on first failure, at compile time);
// 2) to detect cases, when some expression is successfully compiled, but
//    expected, that such expressions must fail to compilate (usually
//    this means, for example, that template lack of appropriate checks, etc...)
//
#define COMPILE_TEST(name, ...) \
    template <typename T, typename = void> struct can_compile_##name : public cant_compile {}; \
    template <typename T> struct can_compile_##name<T, _can_compile<decltype(__VA_ARGS__)> > { constexpr static bool value = true; }; \
    CAN_COMPILE_ASSERT(can_compile_##name<void>::value, name, #__VA_ARGS__); \
    constexpr bool name = can_compile_##name<void>::value


/* Example, how to use COMPILE_TEST() macros with Catch2 test system:

#include "catch2/catch.hpp"
#include "compile_test.h"

struct Copyable {};

struct NonCopyable
{
    NonCopyable(const NonCopyable&) = delete;
};

COMPILE_TEST(Test_MustNotCompile, NonCopyable{TEST_DECLVAL(NonCopyable)});
COMPILE_TEST(Test_MustCompile, Copyable{TEST_DECLVAL(Copyable)});

TEST_CASE("example")
{
    CHECK(!Test_MustNotCompile);
    CHECK(Test_MustCompile);
}

*/
