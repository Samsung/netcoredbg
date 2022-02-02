// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#pragma once
#include <utility>
#include <string>
#include "utils/utility.h"
#include "utils/span.h"
#include "utils/string_view.h"

namespace netcoredbg
{

// This namespace contains implementation details for few following classes,
// contents of this namespace isn't designated for direct use.
namespace EscapedStringInternal
{
    using Utility::string_view;

    // This class allows to perform some predefined actions in moment of time,
    // when temporary object of type TempReference is destroyed. Typically it might
    // be used to perform some sort of lazy evaluation, when some other class accepts
    // it's arguments and temporary TempReference object. And it's expected, that
    // later some evaluation on arguments might be performed. But arguments might
    // be temporary objects, which will be deleted at end of full expression.
    // So arguments should be copied, or evaluation performed, prior deleting
    // the arguments.
    //
    // When const TempReference& used as last (default) argument to
    // class constructor, it is guaranteed, that ~TempReference will be called
    // exactly prior to deleting other temporary arguments, so some required actions
    // might be performed before deleting other arguments.
    //
    // CAUTION:
    // When TempReference is passed to ordinary function (not a class constructor),
    // additionaly cautions is needed: in this case construction order of function
    // arguments isn't predefined, as destruction order (which is reverse). In such
    // situation TempReference might be used only and only in case, when no other 
    // arguments is strictly not a temporary variables. To ensure this, you should
    // delete functions accepting r-values: void f(const Type&&) = delete.
    //
    template <typename T>
    struct TempReference
    {
        // This function should be called from constructor of T, and should pass
        // reference to T's method, which be called on TempReference destruction.
        void set(T* ptr, void (T::*func)()) const noexcept { m_ptr = ptr, m_func = func; }

        // This function might be called from T to cancel callback which is set with `set`
        // function. Especially, `reset` might be called from T's destructor.
        void reset() const noexcept { m_ptr = nullptr; }

        ~TempReference() { if (m_ptr) (m_ptr->*m_func)(); }

    private:
        mutable T* m_ptr;
        mutable void (T::*m_func)();
    };


    // This is actual implementation of `EscapedString` class.
    struct EscapedStringImpl
    {
        // EscapeStrinct class performs type erasure, and information on
        // it's template parameters should be stored in `Params` class.
        struct Params
        {
            string_view forbidden;                  // characters which must be replaced
            Utility::span<const string_view> subst; // strings to which `forbidden` characters must be replaced
            char escape;                            // character, which preceedes each substitution
        };

        using TempRef = TempReference<EscapedStringImpl>;

        // `str` is the source string, in which all `forbidden` characters must be replaced,
        // `isstring` must be set to true only in case, when `str` contains terminating zero
        // (to which `str->end()` points).
        EscapedStringImpl(const Params& params, string_view str, const TempRef& ref, bool isstring);

        ~EscapedStringImpl() { if (m_ref) m_ref->reset(); }

        // see comments in `EscapeString` class below
        void operator()(void *thiz, void (*func)(void*, string_view));
        size_t size() noexcept;
        explicit operator const std::string&();
        operator string_view() noexcept;
        const char* c_str();

    private:
        // This function performs input string (`m_input`) transformation and stores the
        // result in (`m_result`). This function might be called in two cases: from
        // destructor of `TempRef` object (before deleting function argument `str`,
        // to which `m_input` currently points), or in case when result is needed
        // in form of data (on call of any getter function, except of operator()).
        void transform();

        static const size_t UndefinedSize = size_t(0)-1;

        // This TempReference structure was passed as arument to EscapedString class
        // constructor and continue to exist until end of full expression (till ';').
        // At end of full expression all temporary variables destroyed and this structure too.
        // When this happens, TempReference calls transform() method from it's destructor.
        // In transform() method the strings, which was passed as arguments to EscapedString
        // constructor, is transformed and copied in allocated memory, so arguments might
        // be safely deleted. In case, if such transformation and copying occurs before
        // calling of temporary destructors, EscapedStringImpl::transform() calls m_ref->reset()
        // to cancel callback from TempReference class.
        const TempReference<EscapedStringImpl>* m_ref;

        const Params&     m_params;     // character substitution rules
        const string_view m_input;      // points to input string
        std::string       m_result;     // might contain result string (lazy evaluated)
        size_t            m_size;       // size of result stirng (lazy evaluated)
        bool              m_isstring;   // true if m_input->end() points to terminating zero
        bool              m_isresult;   // true if m_result contain result string
    };
} // namespace EscapedStringInternal


/// This class allows lazy transformation of the given source string by substituting
/// set of "forbidden" characters with escape symbol and other substituted character.
/// The main idea is to avoid unwanted memory allocation: when it is possible no
/// memory allocation performed. To achieve this lazy evaluation is used: string
/// transformation is perfomed only if it is needed.
///
/// This class should be used in two steps: first -- creation of class instance from given
/// arguments, at this time neither memory allocation, nor string transformation is
/// performed. Only arguments is remembered. And second step -- calling one of the
/// member functions (see below). At this time string transformation is performed
/// and memory might be allocated. The latter depends on string content, which
/// function and when was called... Memory allocation avoided in cases when no
/// transformation is required (string doesn't contain forbidden characters) and
/// when arguments, given to class constructor, still not destroyed. Also memory
/// allocation always avoided in case of call to `operator()`:  in this case
/// output generated on the fly, but again only if constructor arguments still
/// exists. String is always transformed and copy saved to allocated memory in
/// moment, when constructor arguments destroyed (if `EscapedString` class instance
/// is not temporary variable itself, but continue to exist after end of full expression).
///
template <typename Traits> class EscapedString
{
    using string_view = Utility::string_view;
    using EscapedStringImpl = EscapedStringInternal::EscapedStringImpl;
    static EscapedStringImpl::Params params;

public:
    /// Construct `EscapedString` from c-strings (via implicit conversion) or string_view.
    /// Second parameter must have default value (shouldn't be assigned explicitly).
    EscapedString(string_view str,
        const EscapedStringImpl::TempRef& ref = EscapedStringImpl::TempRef())
    : 
        impl(params, str, ref, false)
    {}

    // Note: there is no reasons to disable temporaries of type std::string, because
    // temporary objects construction and destruction ordef for constructors are
    // predefined, and TempRef always destructed prior to temporary string.

    /// This function allows to avoid memory allocation: functor `func` given as argument
    /// will consume parts of result (transformed string) which will be generated on the fly.
    /// Functor `func` must accept `string_view` as argument.
    template <typename Func, typename = decltype(std::declval<Func>()(std::declval<string_view>()))>
    void operator()(Func&& func) const
    {
        impl.operator()(&func, [](void *thiz, string_view str) { (*static_cast<Func*>(thiz))(str); });
    }

    /// Function returns size of transformed string (no actual transformation performed).
    size_t size() const noexcept { return impl.size(); }

    /// Function transforms string (if it was not transformed earlier) and allocates memory.
    explicit operator const std::string&() const& { return static_cast<const std::string&>(impl); }

    /// Function transforms string and allocates memory.
    operator string_view() const noexcept { return static_cast<string_view>(impl); }

    /// Function transforms string to allocated memory.
    const char* c_str() const    { return impl.c_str(); }

private:
    mutable EscapedStringImpl impl;
};


// instantiation of Params structure for particular Traits template parameter
template <typename Traits> EscapedStringInternal::EscapedStringImpl::Params EscapedString<Traits>::params =
{
    { ((void)([]() -> void {static_assert(sizeof(Traits::forbidden_chars)-1 == Utility::Size(Traits::subst_chars),
        "forbidden_chars and subst_chars must have same size!");}),
      string_view(Traits::forbidden_chars)) },
    { Traits::subst_chars },
    Traits::escape_char
};


/// Implementation of `operator<<`, which allows write `EscapedString` contents to
/// the supplied `std::ostream` avoiding memory allocations.
template <typename Traits>
std::ostream& operator<<(std::ostream& os, const EscapedString<Traits>& estr)
{
    using string_view = Utility::string_view;
    estr([&](string_view str) -> void { os.write(str.data(), str.size()); });
    return os;
}

/// Overloading of `operator+` for `EscapedString`, which allows concatenation
/// of `EscapedString` instances with ordinary strings.
template <typename Traits, typename T>
std::string operator+(const EscapedString<Traits>& left, T&& right)
{
    return static_cast<const std::string&>(left) + std::forward<T>(right);
}

/// Overloading of `operator+` for `EscapedString`, which allows concatenation
/// of `EscapedString` instances with ordinary strings.
template <typename Traits, typename T>
std::string operator+(T&& left, const EscapedString<Traits>& right)
{
    return std::forward<T>(left) + static_cast<const std::string&>(right);
}

} // ::netcoredbg
