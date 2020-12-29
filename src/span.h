// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file span.h  This file contains definition of `span' class, which is similar to std::span from c++20.

#pragma once
#include <utility>
#include <new>
#include <iterator>
#include <assert.h>

namespace netcoredbg
{

namespace Utility
{

/// Class similar to std::span from c++20, but it not implements some other features.
/// See https://en.cppreference.com/w/cpp/container/span for reference.
template <typename T> class span
{
private:
    // Checks, that input argument is STL-containers like std::array, std::string or similar classes.
    template <typename Container, typename Value = typename std::remove_reference<decltype(*std::declval<Container>().data())>::type>
    using is_container = typename std::enable_if<std::is_same<Value, T>::value>;

public:
    typedef T element_type;
    typedef typename std::remove_cv<T>::type value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef pointer iterator;
    typedef const_pointer const_iterator;
    typedef std::reverse_iterator<iterator> reverse_iterator;
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

    /// Constructs an empty span whose data() == nullptr and size() == 0.
    span() noexcept : _first(nullptr), _beyond_last(nullptr) {}

    /// Constructs a span that is a view over the range [first, last)
    span(T* first, T* beyond_last) noexcept : _first(first), _beyond_last(beyond_last) {}

    /// Constructs a span that is a view over the range [ptr, ptr + count),
    /// the resulting span has data() == ptr and size() == count.
    span(T* first, size_t count) noexcept : _first(first), _beyond_last(first + count) {}

    /// @{ Construct span which holds references on container elements.
    // TODO add enable_if and check container properties
    template <class Container, typename = typename is_container<Container>::type>
        span(Container& cont) : span(cont.data(), cont.size()) {}  // TODO add test

    template <class Container, typename = typename is_container<Container>::type>
        span(const Container& cont) : span(cont.data(), cont.size()) {}
    /// @}

    /// Default copy constructor copies the size and data pointer.
    span(const span &) noexcept = default;

    /// Assigns other span to this.
    #ifndef _MSC_VER
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Weffc++"  // avoid bug in gcc (false warning)
    #endif
    span& operator=(const span &other) noexcept { return *new (this) span(other); }
    #ifndef _MSC_VER
    #pragma GCC diagnostic pop
    #endif

    /// Checks if the sequence is empty.
    bool empty() const { return _beyond_last == _first; }

    /// Returns the number of elements in the sequence.
    size_t size() const  { return _beyond_last - _first; }

    /// Returns the size of the sequence in bytes.
    size_t size_bytes() const  { return size() * sizeof(T); }

    /// Accesses an element of the sequence by index.
    /// The behavior is undefined if idx is out of range.
    T& operator[](size_t idx) const noexcept
    {
        // allowing access to cell beyond of data for address arithmetics
        return assert(idx <= size()), _first[idx];
    }

    /// Access the first element. Calling `front' on an empty span results in undefined behavior
    T& front() const noexcept  { return assert(_first), *_first; }

    /// Access the last element. Calling `back' on an empty span results in undefined behavior
    T& back() const noexcept { return assert(_beyond_last), _beyond_last[-1]; }

    /// Returns a pointer to the beginning of the sequence.
    T *data() const noexcept { return /*assert(_first),*/ _first; }

    /// Obtains a span that is a view over the `count' elements of this span starting at `offset'.
    /// The behavior is undefined if either `offset' or `count' is out of range.
    span subspan(size_t offset, size_t count = size_t(-1)) const noexcept
    {
        assert(offset <= size());
        assert(count == size_t(-1) || offset + count <= size());
        return span(_first + offset, count == size_t(-1) ? _beyond_last - (_first + offset) : count);
    }

    /// Obtains a subspan consisting of the first N elements of the sequence.
    span first(size_t count) const { return subspan(0, count); }

    /// Obtains a subspan consisting of the last N elements of the sequence.
    span last(size_t count) const { return subspan(size() - count, count); }

    /// Returns an iterator to the beginning of the sequence.
    /// If the span is empty, the returned iterator will be equal to end().
    iterator begin() const { return _first; }

    /// Returns an iterator to the element following the last element of the span.
    iterator end() const { return _beyond_last; }

    /// Returns an iterator to the beginning of the sequence (const version).
    const_iterator cbegin() const { return _first; }

    /// Returns an iterator to the element following the last element of the span.
    const_iterator cend() const { return _beyond_last; }

    /// Returns a reverse iterator to the beginning of the sequence. // TODO add tests
    reverse_iterator rbegin() const { return end(); }

    /// Returns a reverse iterator to the end of the sequence.
    reverse_iterator rend() const { return begin(); }

    /// Returns a const reverse iterator to the beginning of the sequence.
    const_reverse_iterator crbegin() const { return cend(); }

    /// Returns a reverse iterator to the end of the sequence.
    const_reverse_iterator crend() const { return cbegin(); }

private:
    T *_first, *_beyond_last;
};

} // Utility namespace
} // netcoredbg
