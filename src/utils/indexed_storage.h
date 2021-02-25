// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#pragma once
#include <cstddef>
#include <utility>
#include <vector>
#include <limits>
#include <algorithm>

namespace netcoredbg
{

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

} // ::netcoredbg
