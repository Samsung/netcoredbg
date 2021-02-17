// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

// This file contains few supplimentary header-only classes and functions.

#pragma once
#include <stddef.h>
#include <vector>
#include <utility>
#include <algorithm>
#include <type_traits>

namespace netcoredbg
{

namespace Utility
{

/// @{ This function is similar to `std::size()` from C++17 and allows to determine
/// the object size as number of elements, which might be stored within object
/// (opposed to sizeof(), which returns object sizes in bytes). Typically these
/// functions applicable to arrays and to classes like std::array.
template <typename T> constexpr auto Size(const T& v) -> decltype(v.size()) { return v.size(); }
template <class T, size_t N> constexpr size_t Size(const T (&)[N]) noexcept { return N; }
/// @}

  
/// This type is similar to `std::index_sequence` from C++14 and typically
/// should be used in pair with `MakeSequence` type to create sequence of
/// indices usable in template metaprogramming techniques.
template <size_t... Index> struct Sequence {};

// Actual implementation of MakeSequence type.
namespace Internals
{
    template <size_t Size, size_t... Index> struct MakeSequence : MakeSequence<Size-1, Size-1, Index...> {};
    template <size_t... Index> struct MakeSequence<0, Index...> { typedef Sequence<Index...> type; };
}

/// MakeSequence type is similar to `std::make_index_sequence<N>` from C++14.
/// Instantination of this type exapands to `Sequence<size_t...N>` type, where
/// `N` have values from 0 to `Size-1`.
template <size_t Size> using MakeSequence = typename Internals::MakeSequence<Size>::type;


/// @{ This is similar to std:void_t which is defined in c++17.
template <typename... Args> struct MakeVoid { typedef void type; };
template <typename... Args> using Void = typename MakeVoid<Args...>::type;
/// @}


// This is helper class which simplifies implementation of singleton classes.
//
// Usage example:
//   1) define dictinct type of singleton: typedef Singleton<YourType> YourSingleton;
//   2) to access your singleton use expression: YourSingleton::instance().operations...
//
template <typename T> struct Singleton
{
    static T& instance()
    {
        static T val;
        return val;
    }
};


// This is helper class, which simplifies creation of custom scalar types
// (ones, which provide stron typing and disallow mixing with any other scalar types).
// Basically these types support equality compare operators and operator<
// (to allow using such types with STL containers).
//
template <typename T> struct CustomScalarType
{
    friend bool operator==(T a, T b) { return static_cast<typename T::ScalarType>(a) == static_cast<typename T::ScalarType>(b); }
    template <typename U> friend bool operator==(T a, U b) { return static_cast<typename T::ScalarType>(a) == b; }
    template <typename U> friend bool operator==(U a, T b) { return a  == static_cast<typename T::ScalarType>(b); }
    friend bool operator!=(T a, T b) { return !(a == b); }
    template <typename U> friend bool operator!=(T a, U b) { return !(a == b); }
    template <typename U> friend bool operator!=(U a, T b) { return !(a == b); }

    bool operator<(const T& other) const
    {
        return static_cast<typename T::ScalarType>(static_cast<const T&>(*this)) < static_cast<typename T::ScalarType>(static_cast<const T&>(other));
    }
};


// This class implements container, which hold elements of type `T', where
// elements addressed by integral value of type `Key'. The `Key' can take
// values from 0 to `Max' inclusively. The value of `Key' is not provided
// initially, but automatically assigned when new value of type `T' is
// added to container. Later, this value might be accessed by using assigned
// key value. This container type doesn't allows duplicate values of type `T':
// for each unique `T' value corresponds only one unique `Key' value.
// This container tries avoid using of repeated `Key' value after call
// to `clear()' (it uses cyclically increased number, which wraps around
// after reaching `Max' value).
template <typename Key, typename T, Key Max = std::numeric_limits<Key>::max()>
class IndexedStorage
{
public:
    // Data type used as the key.
    typedef Key key_type;

    // Data type stored in container.
    typedef T mapped_type;

    // Data type, which combines key with element stored in container.
    typedef std::pair<key_type, mapped_type> value_type;

    // These definitions needed for standard library algoithms.
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;
    typedef const value_type& reference;
    typedef const value_type* pointer;

    // This class implemens at lest input-iterator
    // which allows to iterate over all elements stored in container.
    typedef typename std::vector<value_type>::const_iterator iterator;

    // Return iterator pointing on first element.
    iterator begin() const { return m_data.begin(); }

    // Return iterator pointing beyond last element.
    iterator end() const { return m_data.end(); }

    // Constructor which creates new empty container.
    IndexedStorage() {}

    // Return number of elements currently stored in container.
    size_type size() const { return m_data.size(); }

    // Erase all contents.
    void clear()
    {
        m_base += key_type(m_data.size());
        m_data.clear();
    }

    // This function creates new element from supplied arguments and returns
    // pair containing iterator pointing to new created element and boolean
    // value which is false when element already present in container or true
    // if new element was created.
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        mapped_type data {std::forward<Args&&>(args)...};
        return insert(std::move(data));
    }

    // This function inserts new element to container and returning pair
    // consisting of an iterator pointing to element which was inserted into
    // the container and second element of the pair is boolean value which is
    // set to false if element was already present in the container or set to
    // true, if new element was added.
    std::pair<iterator, bool> insert(const value_type& val)
    {
        iterator it = do_insert(val);
        if (it != m_data.end()) return {it, false};
        m_data.push_back(value_type(next_id(), val));
        return {--m_data.end(), true};
    }

    // This function do the same as previous, but avoid copying of `val'.
    std::pair<iterator, bool> insert(mapped_type&& val)
    {
        iterator it = do_insert(val);
        if (it != m_data.end()) return {it, false};
        m_data.push_back(value_type(next_id(), std::move(val)));
        return {--m_data.end(), true};
    }

    // This functions find data element which corresponds to supplied `key'
    // and returns iterator pointing to it, or returns iterator pointing
    // to `end()` value of no any element corresponds to supplied `key'.
    iterator find(key_type key) const
    {
        key_type index = key - m_base;
        if (index >= m_data.size() || index < 0 || m_data[index].first != key)
            return end();

        return begin() + index;
    }

    // This function checks if element with corresponding `key' value present in the container.
    bool contains(key_type key) const { return find(key) != end(); }

private:
    key_type m_base;
    std::vector<value_type> m_data;

    iterator do_insert(const mapped_type& val)
    {
        return std::find_if(m_data.cbegin(), m_data.cend(),
            [&](const value_type& other){ return other.second == val; });
    }

    key_type next_id() const
    {
        key_type next_id = m_base + key_type(m_data.size());
        if (next_id > Max) next_id -= Max;
        return next_id;
    }
};

} // Utility namespace

} // namespace netcoredbg
