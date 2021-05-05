// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#pragma once
#include "utils/string_view.h"

namespace netcoredbg
{

namespace Utility
{

/// Class which allows to wrap string literals and provide operator+ to concatenate
/// string literals at compile time. For example: literal("aaa") + literal("bbb")...
/// Result is the constexpr object of type LiteralString<Size>.
template <size_t Size>
struct LiteralString
{
    /// return stirng size, in bytes
    constexpr size_t size() const { return Size - 1; }

    /// return null-terminated representation of string
    constexpr const char *c_str() const { return m_data; }

    /// returns reference to character with specified `index`
    constexpr char const& operator[](size_t index) const { return m_data[index]; }

    /// string literals convertible to string_view
    constexpr operator string_view() const { return {c_str(), size()}; }

    // This function not indentent for direct use, it concatenates two string
    // literals. User code should use operator+ for that.
    template <size_t LSize, size_t RSize, size_t... LIndex, size_t... RIndex>
    constexpr LiteralString(const LiteralString<LSize>& left, const LiteralString<RSize>& right, Sequence<LIndex...>, Sequence<RIndex...>)
    : m_data{left[LIndex]..., right[RIndex]..., 0}
    {}

    // Constructor isn't indentent for direct use, user code should use `literal("...")` function.
    template <size_t... Idx>
    constexpr LiteralString(const char (&str)[Size], Sequence<Idx...>)
    : m_data{str[Idx]...}
    {}

private:
    const char m_data[Size];
};

/// Overload of operator+ for `LiteralString<Size>` class: this function concatenates
/// two string literals at compile time and returns another string literal.
template <size_t LSize, size_t RSize, typename Result = LiteralString<RSize + LSize - 1> >
constexpr Result operator+(const LiteralString<LSize>& left, const LiteralString<RSize>& right)
{
    return Result(left, right, MakeSequence<LSize-1>{},  MakeSequence<RSize-1>{});
}

/// This function should be used to create string literals. Function receives
/// constexpr char arrays (literal values or variables) as argument.
template <size_t Size>
constexpr LiteralString<Size> literal(const char (&str)[Size])
{
    return LiteralString<Size>(str, MakeSequence<Size>{});
}

} // Utility
} // ::netcoredbg
