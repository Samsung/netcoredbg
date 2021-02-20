// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

/// \file  string_view.h  This file contains replacement of std::string_view class for pre c++17 compilers.

#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <iterator>
#include <ostream>
#include <string>

namespace netcoredbg { namespace Utility {

template <typename CharT> class StringViewBase
{
public:
	typedef CharT             value_type;
	typedef value_type*       pointer;
	typedef const value_type* const_pointer;
	typedef const value_type&       reference;  // TODO const due to reverse iterator
	typedef const value_type& const_reference;
	typedef ptrdiff_t         difference_type;
	typedef size_t            size_type;

private:
	struct c_str_wrapper
	{
		const_pointer str;
		constexpr c_str_wrapper(const_pointer str) : str(str) {}
	};

	template <typename U, typename Dummy = void> struct Traits
	{
		static size_type length(const U *ptr)
		{
			const U *start = ptr;
			while (*ptr != 0) ++ptr;
			return ptr - start;
		}

		static int compare(const StringViewBase<U>& a, const StringViewBase<U>& b)
		{
			iterator i = a.begin(), j = b.begin();
			while (i != a.end() && j != b.end())
			{
				int r = *i - *j;
				if (r != 0)
					return r < 0 ? -1 : +1;
				++i, ++j;
			}

			return a.size() < b.size() ? -1 : (a.size() > b.size()) ? +1 : 0;
		}

		static const U* find_first(const StringViewBase<U>& s, U c)
		{
			iterator i = s.begin();
			while (i != s.end())
			{
				if (*i == c)
					return &*i;
				++i;
			}
			return NULL;
		}

		static const U* find_last(const StringViewBase<U>& s, U c)
		{
			reverse_iterator i = s.rbegin();
			while (i != s.rend())
			{
				if (*i == c)
					return &*i;
				++i;
			}
			return NULL;
		}
	};

	template <typename CharType> struct CharTraits
	{
		static size_type length(const CharType *ptr) { return strlen(ptr); }

		static int compare(const StringViewBase<CharType>& a, const StringViewBase<CharType>& b)
		{
			int result = memcmp(a.data(), b.data(), a.size() < b.size() ? a.size() : b.size());
			if (result != 0) return result;

			return a.size() < b.size() ? -1 : (a.size() > b.size()) ? +1 : 0;
		}

		static const CharType* find_first(const StringViewBase<CharType>& s, CharType c)
		{
			return static_cast<const_pointer>(memchr(s.data(), c, s.size()));
		}

		static const CharType* find_last(const StringViewBase<CharType>& s, CharType c)
		{
			return Traits<CharType, CharType>::find_last(s, c);
		}
	};

	template <typename Dummy> struct Traits<char, Dummy> : public CharTraits<char> {};
	template <typename Dummy> struct Traits<unsigned char, Dummy> : public CharTraits<unsigned char> {};

	const_pointer _data;
	const_pointer _end;

public:
	class const_iterator
	{
	public:
		typedef StringViewBase::value_type      value_type;
		typedef StringViewBase::size_type       difference_type;
		typedef StringViewBase::pointer         pointer;
		typedef StringViewBase::const_pointer   const_pointer;
		typedef StringViewBase::reference       reference;
		typedef StringViewBase::const_reference const_reference;
		typedef std::random_access_iterator_tag iterator_category;

		const_iterator() : p(NULL) {}
		const_iterator(const const_iterator& other) : p(other.p) {}
		const_iterator& operator=(const const_iterator& other) { return p = other.p, *this; }

		bool operator==(const_iterator other) const { return p == other.p; }
		bool operator!=(const_iterator other) const { return p != other.p; }

		const_reference operator*() const { return *p; }
		const_pointer operator->() const { return p; }

		const_iterator& operator++() { return ++p, *this; }
		const_iterator operator++(int) { return p++; }
		const_iterator& operator--() { return --p, *this; }
		const_iterator operator--(int) { return p--; }

		const_iterator operator+(difference_type n) const { return p + n; }
		const_iterator operator-(difference_type n) const { return p - n; }

		difference_type operator-(const_iterator other) const { return p - other.p; }

		const_iterator& operator+=(difference_type n) { return p += n, *this; }
		const_iterator& operator-=(difference_type n) { return p -= n, *this; }

		bool operator<(const_iterator other) const { return p < other.p; }
		bool operator>(const_iterator other) const { return p > other.p; }
		bool operator>=(const_iterator other) const { return p >= other.p; }
		bool operator<=(const_iterator other) const { return p <= other.p; }

		value_type operator[](difference_type n) const { return p[n]; }
	private:
		const_pointer p;

		friend class StringViewBase;
		const_iterator(const_pointer p) : p(p) {}
	};

	typedef const_iterator iterator;
	typedef std::reverse_iterator<const_iterator> const_reverse_iterator;
	typedef const_reverse_iterator reverse_iterator;

	/// This is a special value equal to the maximum value representable by the type size_type.
	/// The exact meaning depends on context, but it is generally used either as end of view
	/// indicator by the functions that expect a view index or as the error indicator by the
	/// functions that return a view index.
	static const size_type npos = size_type(-1);

	/// Default constructor. Constructs an empty basic_string_view. After construction, data()
	/// is equal to nullptr, and size() is equal to 0.
	constexpr StringViewBase() : _data(0), _end(0) {}

	constexpr StringViewBase(const StringViewBase& other) : _data(other._data), _end(other._end) {}

	/// Constructs a view of the first count characters of the character array starting with the
	/// element pointed by `ptr'. String `ptr' can contain null characters.
	constexpr StringViewBase(const_pointer ptr, size_type size) : _data(ptr), _end(_data + size) {}

	// using of wrapper, instead const char*, allows avoid of computing strlen() for string literals
	/// Construct string_view from c-string.
	constexpr StringViewBase(c_str_wrapper str)
            : _data(str.str), _end(_data + Traits<value_type>::length(_data)) {}

	/// Construct string_view from literal (no need to compute length).
	template <size_type N>
        /*explicit*/ constexpr StringViewBase(const value_type (&literal)[N])
        : _data(literal), _end(_data + N - 1) {}

	/// Construct string_view from variable-sized buffer (need to compute length).
	template <size_type N>
        explicit constexpr StringViewBase(value_type (&literal)[N])
            : _data(literal), _end(_data + Traits<value_type>::length(_data)) {}

        /// Construct string_view from std::string.
        template <typename Traits, typename Allocator>
	constexpr StringViewBase(const std::basic_string<CharT, Traits, Allocator>& str)
            : _data(&str[0]), _end(_data + str.size()) {}

        /// Conversion to std::string.
        template <typename Traits, typename Allocator>
        operator std::basic_string<CharT, Traits, Allocator>() const
        {
            return {data(), size()};
        }

	/// Replaces the view with that of view.
	StringViewBase& operator=(const StringViewBase& other) { return *new (this) StringViewBase(other); }

	///@{ returns an iterator to the beginning
	const_iterator begin() const { return _data; }
	const_iterator cbegin() const { return begin(); }
	///@}

	///@{ returns an iterator to the end
	const_iterator end() const { return _end; }
	const_iterator cend() const { return end(); }
	///@}

	///@{ returns a reverse iterator to the beginning
	const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
	const_reverse_iterator crbegin() const { return rbegin(); }
	///@}

	///@{ returns a reverse iterator to the end
	const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
	const_reverse_iterator crend() const { return rend(); }
	///@}

	/// accesses the specified character
	const_reference operator[](size_type n) const { return _data[n]; }

	/// accesses the specified character with bounds checking
	const_reference at(size_type n) const { assert(n < size_type(_end - _data));  return _data[n]; }

	/// accesses the first character
	const_reference front() const { return _data[0]; }

	/// accesses the last character
	const_reference back() const { return _end[-1]; }

	/// Returns a pointer to the underlying character array. The pointer is such that
	/// the range [data(); data() + size()) is valid and the values in it correspond
	/// to the values of the view.
	constexpr const_pointer data() const { return _data; }

	///@{ Functions returns the number of characters in the view,
	constexpr size_type size() const { return _end - _data; }
	constexpr size_type length() const { return size(); }
	///@}

	/// The largest possible number of chars that can be referred to by a string_view.
	constexpr size_type max_size() const { return SIZE_MAX; }

	/// Checks if the view has no characters, i.e. whether size() == 0.
	constexpr bool empty() const { return _data == _end; }

	/// Moves the start of the view forward by n characters.
	/// The behavior is undefined if n > size().
	void remove_prefix(size_type n) { _data += n; }

	/// Moves the end of the view back by n characters.
	/// The behavior is undefined if n > size().
	void remove_suffix(size_type n) { _end -= n; }

	/// Exchanges the view with that of v.
	void swap(StringViewBase& other) { StringViewBase copy(other); other = *this, *this = copy; }

	/// Function returns a view of the substring [pos, pos + rcount), where rcount is the
	/// smaller of count and size() - pos.
	StringViewBase substr(size_type pos = 0, size_type count = npos) const
	{
		assert(pos <= size());
		return StringViewBase(&_data[pos], count < size() - pos ? count : size() - pos);
	}


	/// Function copies the substring [pos, pos + rcount) to the character array pointed
	/// to by dest, where rcount is the smaller of count and size() - pos.
	/// Return value: Number of characters copied.
	size_type copy(pointer dest, size_type count, size_type pos = 0) const
	{
		StringViewBase s(substr(pos, count));
		memcpy(dest, s.data(), s.size() * sizeof(value_type));  // ok for wchar_t, char_32, etc...
		return s.size();
	}


	///@{ Function compares two character sequences.
	/// Return value: negative value if this view is less than the other character sequence, zero
	/// if the both character sequences are equal, positive value if this view is greater than
	/// the other character sequence.

	int compare(StringViewBase s) const { return Traits<value_type>::compare(*this, s); }

	int compare(size_type pos1, size_type count1, StringViewBase s) const
	{
		return substr(pos1, count1).compare(s);
	}

	int compare(size_type pos1, size_type count1, StringViewBase s, size_type pos2, size_type count2) const
	{
		return substr(pos1, count1).compare(s.substr(pos2, count2));
	}

	int compare(const_pointer s) const { return compare(StringViewBase(s)); }

	int compare(size_type pos1, size_type count1, const_pointer s) const
	{
		return substr(pos1, count1).compare(StringViewBase(s));
	}

	int compare(size_type pos1, size_type count1, const_pointer s, size_type count2) const
	{
		return substr(pos1, count1).compare(StringViewBase(s, count2));
	}

	///@}

	///@{ Function finds the first substring equal to the given character sequence.
	/// Return value: Position of the first character of the found substring, or npos if no such substring is found

	size_type find(value_type c, size_type pos = 0) const
	{
		StringViewBase h(substr(pos));
		const_pointer p = Traits<value_type>::find_first(h, c);
		return p == NULL ? npos : p - data();
	}

	size_type find(StringViewBase s, size_type pos = 0) const
	{
		if (s.empty()) return 0;

		while (pos = find(s[0], pos), pos != npos)
		{
			if (! compare(pos, s.size(), s))
				return pos;

			++pos;
		}
		return npos;
	}

	size_type find(const_pointer s, size_type pos, size_type count) const { return find(StringViewBase(s, count), pos); }

	size_type find(const_pointer s, size_type pos = 0) const { return find(StringViewBase(s), pos); }

	///@}

	///@{ Function finds the last substring equal to the given character sequence.
	/// Return value: Position of the first character of the found substring or npos if no such substring is found.

	size_type rfind(value_type c, size_type pos = npos) const
	{
		StringViewBase h(substr(0, pos));
		const_pointer p = Traits<value_type>::find_last(h, c);
		return p == NULL ? npos : p - data();
	}

	size_type rfind(StringViewBase s, size_type pos = npos) const
	{
		if (s.empty()) return 0;

		StringViewBase h(*this);
		size_type p;
		while (p = h.rfind(s[0], pos), p != npos)
		{
			if (! compare(p, s.size(), s))
				return p;

			h = h.substr(0, p);
		}
		return npos;
	}

	size_type rfind(const_pointer s, size_type pos, size_type count) const { return rfind(StringViewBase(s, count), pos); }

	size_type rfind(const_pointer s, size_type pos = npos) const { return rfind(StringViewBase(s), pos); }

	///@}


	///@{ Function finds the first character equal to any of the characters in the given character sequence.
	/// Return value: Position of the first occurrence of any character of the substring, or npos if no such character is found.

	size_type find_first_of(value_type c, size_type pos = 0) const { return find(c, pos); }

	size_type find_first_of(StringViewBase s, size_type pos = 0) const
	{
		for (iterator i = begin() + pos; i != end(); ++i)
		{
			if (s.find(*i) != npos)
				return i - begin();
		}
		return npos;
	}

	size_type find_first_of(const_pointer s, size_type pos, size_type count) const
	{
		return find_first_of(StringViewBase(s, count), pos);
	}

	size_type find_first_of(const_pointer s, size_type pos = 0) const { return find_first_of(StringViewBase(s), pos); }

	///@}

	///@{ Function finds the last character equal to one of characters in the given character sequence.
	/// Exact search algorithm is not specified. The search considers only the interval [0; pos].
	/// If the character is not present in the interval, npos will be returned.
	///
	/// Return value: Position of the last occurrence of any character of the substring, or npos if no such character is found.

	size_type find_last_of(value_type c, size_type pos = npos) const { return rfind(c, pos); }

	size_type find_last_of(StringViewBase s, size_type pos = npos) const
	{
		if (begin() != end())
		{
			iterator i = (pos == npos) ? end() - 1 : begin() + pos;
			do {
				if (s.find(*i) != npos)
					return i - begin();

			} while (i-- != begin());
		}
		return npos;
	}

	size_type find_last_of(const_pointer s, size_type pos, size_type count) const
	{
		return find_last_of(StringViewBase(s, count), pos);
	}

	size_type find_last_of(const_pointer s, size_type pos = npos) const { return find_last_of(StringViewBase(s), pos); }

	///@}

	///@{ Function finds the first character not equal to any of the characters in the given character sequence
	///
	/// Return value: Position of the first character not equal to any of the characters in the
	/// given string, or npos if no such character is found.

	size_type find_first_not_of(StringViewBase s, size_type pos = 0) const
	{
		for (iterator i = begin() + pos; i != end(); ++i)
		{
			if (s.find(*i) == npos)
				return i - begin();
		}
		return npos;
	}

	size_type find_first_not_of(value_type c, size_type pos = 0) const
	{
		return find_first_not_of(StringViewBase(&c, 1), pos);
	}

	size_type find_first_not_of(const_pointer s, size_type pos, size_type count) const
	{
		return find_first_not_of(StringViewBase(s, count), pos);
	}

	size_type find_first_not_of(const_pointer s, size_type pos = 0) const
	{
		return find_first_not_of(StringViewBase(s), pos);
	}

	///@}

	///@{ Function finds the last character not equal to any of the characters in the given character sequence.
	//
	// Return value: Position of the last character not equal to any of the characters in the
	// given string, or npos if no such character is found.

	size_type find_last_not_of(StringViewBase s, size_type pos = npos) const
	{
		if (begin() != end())
		{
			iterator i = (pos == npos) ? end() - 1 : begin() + pos;
			do {
				if (s.find(*i) == npos)
					return i - begin();

			} while (i-- != begin());
		}
		return npos;
	}

	size_type find_last_not_of(value_type c, size_type pos = npos) const
	{
		return find_last_not_of(StringViewBase(&c, 1), pos);
	}

	size_type find_last_not_of(const_pointer s, size_type pos, size_type count) const
	{
		return find_last_not_of(StringViewBase(s, count), pos);
	}

	size_type find_last_not_of(const_pointer s, size_type pos = npos) const
	{
		return find_last_not_of(StringViewBase(s), pos);
	}

	///@}

        /// @{ Following functions check, that string starts with specified substring.

        bool starts_with(StringViewBase prefix) const
        {
            return substr(0, prefix.size()) == prefix;
        }

        bool starts_with(value_type c) const { return !empty() && front() == c; }

        // for c-strings length is computed
        bool starts_with(c_str_wrapper str) const
        {
            return starts_with({str.str, Traits<value_type>::length(str.str)});
        }

        // for literals length is known
        template <size_type N>
        bool starts_with(const value_type (&literal)[N])
        {
            return starts_with({literal, N - 1});
        }

        // for arrays length is computed
        template <size_type N>
        bool starts_with(value_type (&literal)[N])
        {
            return starts_with({literal, Traits<value_type>::length(literal)});
        }

        /// @}

        /// @{ Following functions check, that string ends with specfied substring.

        bool ends_with(StringViewBase suffix) const
        {
            return size() >= suffix.size() && compare(size() - suffix.size(), npos, suffix) == 0;
        }

        bool ends_with(value_type c) const { return !empty() && back() == c; }

        // for c-strings length is computed
        bool ends_with(c_str_wrapper str) const
        {
            return ends_with({str.str, Traits<value_type>::length(str.str)});
        }

        // for literals length is known
        template <size_type N>
        bool ends_with(const value_type (&literal)[N])
        {
            return ends_with({literal, N - 1});
        }

        // for arrays length is computed
        template <size_type N>
        bool ends_with(value_type (&literal)[N])
        {
            return ends_with({literal, Traits<value_type>::length(literal)});
        }

        /// @}

        /// @{ Following functions check, if string contains specified substring.

        bool contains(StringViewBase substr) const
        {
            return size() >= substr.size() && find(substr) != npos;
        }

        bool contains(value_type c) const { return !empty() && find(c) != npos; }

        // for c-strings length is computed
        bool contains(c_str_wrapper str) const
        {
            return contains({str.str, Traits<value_type>::length(str.str)});
        }

        // for literals length is known
        template <size_type N>
        bool contains(const value_type (&literal)[N])
        {
            return contains({literal, N - 1});
        }

        // for arrays length is computed
        template <size_type N>
        bool contains(value_type (&literal)[N])
        {
            return contains({literal, Traits<value_type>::length(literal)});
        }

        /// @}
};

template <typename T> const typename StringViewBase<T>::size_type StringViewBase<T>::npos;

typedef StringViewBase<char> string_view;
typedef StringViewBase<unsigned char> u8string_view;
typedef StringViewBase<char16_t> u16string_view;
typedef StringViewBase<char32_t> u32string_view;
typedef StringViewBase<wchar_t> wstring_view;


///@{ Compares two views. All comparisons are done via the compare() member function
/// The implementation shall provide sufficient additional overloads of these functions so that a
/// string_view object sv may be compared to another object T with an implicit conversion to string_view,
/// with semantics identical to comparing sv and basic_string_view(t).

template <typename CharT> bool operator==(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) == 0; }
template <typename CharT> bool operator!=(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) != 0; }
template <typename CharT> bool operator<(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) < 0; }
template <typename CharT> bool operator<=(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) <= 0; }
template <typename CharT> bool operator>(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) > 0; }
template <typename CharT> bool operator>=(StringViewBase<CharT> a, StringViewBase<CharT> b) { return a.compare(b) >= 0; }

template <typename CharT, typename T> bool operator==(StringViewBase<CharT> a, const T& b) { return a.compare(b) == 0; }
template <typename CharT, typename T> bool operator==(const T& a, StringViewBase<CharT> b) { return b.compare(a) == 0; }

template <typename CharT, typename T> bool operator!=(StringViewBase<CharT> a, const T& b) { return a.compare(b) != 0; }
template <typename CharT, typename T> bool operator!=(const T& a, StringViewBase<CharT> b) { return b.compare(a) != 0; }

template <typename CharT, typename T> bool operator<(StringViewBase<CharT> a, const T& b) { return a.compare(b) < 0; }
template <typename CharT, typename T> bool operator<(const T& a, StringViewBase<CharT> b) { return b.compare(a) >= 0; }

template <typename CharT, typename T> bool operator<=(StringViewBase<CharT> a, const T& b) { return a.compare(b) <= 0; }
template <typename CharT, typename T> bool operator<=(const T& a, StringViewBase<CharT> b) { return b.compare(a) > 0; }

template <typename CharT, typename T> bool operator>(StringViewBase<CharT> a, const T& b) { return a.compare(b) > 0; }
template <typename CharT, typename T> bool operator>(const T& a, StringViewBase<CharT> b) { return b.compare(a) <= 0; }

template <typename CharT, typename T> bool operator>=(StringViewBase<CharT> a, const T& b) { return a.compare(b) >= 0; }
template <typename CharT, typename T> bool operator>=(const T& a, StringViewBase<CharT> b) { return b.compare(a) < 0; }

///@}

/// This allows to output string_view to ostream...
inline std::ostream& operator<<(std::ostream& os, string_view str)
{
    os.write(str.data(), str.size());
    return os;
}

}}  // ::netcoredbg::Utility
