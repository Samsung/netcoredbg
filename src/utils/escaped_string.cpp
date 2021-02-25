// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <algorithm>
#include <string>
#include "escaped_string.h"
#include "assert.h"

namespace netcoredbg
{

EscapedStringInternal::EscapedStringImpl::EscapedStringImpl(const EscapedStringInternal::EscapedStringImpl::Params& params, Utility::string_view str, const TempRef& ref, bool isstring)
: 
    m_ref(&ref), m_params(params), m_input(str), m_result(), m_size(UndefinedSize), m_isstring(isstring), m_isresult(false)
{
    ref.set(this, &EscapedStringImpl::transform);
}

// This function performs input string transformation according to specified (via `m_params) rules.
// Result will be passed to supplied callback function.
void EscapedStringInternal::EscapedStringImpl::operator()(void *thiz, void (*func)(void*, Utility::string_view))
{
    // always have transformed result
    if (m_isresult)
        return func(thiz, {&m_result[0], m_result.size()});

    // case, when no conversion needed
    if (m_size == m_input.size())
        return func(thiz, m_input);

    // perform transformation and compute result size
    size_t size = 0;
    string_view src = m_input;
    while (!src.empty())
    {
        // try to find first forbidden character
        auto it = std::find_first_of(src.begin(), src.end(), m_params.forbidden.begin(), m_params.forbidden.end());
        size_t prefix_size = it - src.begin();
        if (prefix_size)
        {
            // output any other charactes that preceede first forbidden character
            func(thiz, src.substr(0, prefix_size));
            size += prefix_size;
        }

        if (it != src.end())
        {
            // find right substitution for forbidden character and output substituting pair of characters
            auto ir = std::find(m_params.forbidden.begin(), m_params.forbidden.end(), *it);
            char s[2] = {m_params.escape, m_params.subst[ir - m_params.forbidden.begin()]};
            func(thiz, {s, 2});
            size += 2;
            prefix_size += 1;
        }

        src.remove_prefix(prefix_size);
    }

    // remember output size (to avoid computations in future)
    if (m_size == UndefinedSize)
        m_size = size;
}

// function computes output size (but not produces the output)
size_t EscapedStringInternal::EscapedStringImpl::size() noexcept
{
    if (m_size == UndefinedSize)
        (*this)(nullptr, [](void *, string_view){});

    return m_size;
}

// function allocates memory and transforms string in all cases,
// except of case, when transformation isn't required at all and
// input arguments still not destroyed.
EscapedStringInternal::EscapedStringImpl::operator Utility::string_view() noexcept
{
    if (! m_isresult)
    {
        if (size() == m_input.size())
            return m_input;

        transform();
    }
    return {&m_result[0], m_result.size()};
}

// function allocates memory and transforms string.
EscapedStringInternal::EscapedStringImpl::operator const std::string&()
{
    if (!m_isresult)
        transform();

    return m_result;
}

// function allocates memory and transforms string in all cases,
// except of case, when transformation isn't required at all, and
// input arguments still not destroyed, and input argument contains
// terminating zero.
const char* EscapedStringInternal::EscapedStringImpl::c_str()
{
    if (m_isstring && !m_isresult && size() == m_input.size())
        return m_input.data();
    else
        return static_cast<const std::string&>(*this).c_str();
}

// function performs string transformation to allocated memory
void EscapedStringInternal::EscapedStringImpl::transform()
{
    m_ref->reset();
    m_ref = nullptr;

    m_result.resize(size(), 0);
    auto it = m_result.begin();

    auto func = [&](string_view str)
    {
        m_result.replace(it, it + str.size(), str.begin(), str.end());
        it += str.size();
    };

    (*this)(&func, [](void *fp, string_view str) { (*static_cast<decltype(func)*>(fp))(str); });

    m_isresult = true;
    m_isstring = true;
}

} // ::netcoredbg::Utility
