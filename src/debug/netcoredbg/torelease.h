// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

// This class acts a smart pointer which calls the Release method on any object
// you place in it when the ToRelease class falls out of scope.  You may use it
// just like you would a standard pointer to a COM object (including if (foo),
// if (!foo), if (foo == 0), etc) except for two caveats:
//     1. This class never calls AddRef and it always calls Release when it
//        goes out of scope.
//     2. You should never use & to try to get a pointer to a pointer unless
//        you call Release first, or you will leak whatever this object contains
//        prior to updating its internal pointer.
template<class T>
class ToRelease
{
public:
    ToRelease()
        : m_ptr(NULL)
    {}

    ToRelease(T* ptr)
        : m_ptr(ptr)
    {}

    ~ToRelease()
    {
        Release();
    }

    void operator=(T *ptr)
    {
        Release();

        m_ptr = ptr;
    }

    T* operator->()
    {
        return m_ptr;
    }

    operator T*()
    {
        return m_ptr;
    }

    T** operator&()
    {
        return &m_ptr;
    }

    T* GetPtr() const
    {
        return m_ptr;
    }

    T* Detach()
    {
        T* pT = m_ptr;
        m_ptr = NULL;
        return pT;
    }

    void Release()
    {
        if (m_ptr != NULL)
        {
            m_ptr->Release();
            m_ptr = NULL;
        }
    }

    ToRelease(ToRelease&& that) noexcept : m_ptr(that.m_ptr) { that.m_ptr = nullptr; }
private:
    ToRelease(const ToRelease& that) = delete;
    T* m_ptr;
};

#ifndef IfFailRet
#define IfFailRet(EXPR) do { Status = (EXPR); if(FAILED(Status)) { return (Status); } } while (0)
#endif

#ifndef _countof
#define _countof(x) (sizeof(x)/sizeof(x[0]))
#endif

#ifdef PAL_STDCPP_COMPAT
#define _iswprint   PAL_iswprint
#define _wcslen     PAL_wcslen
#define _wcsncmp    PAL_wcsncmp
#define _wcsrchr    PAL_wcsrchr
#define _wcscmp     PAL_wcscmp
#define _wcschr     PAL_wcschr
#define _wcscspn    PAL_wcscspn
#define _wcscat     PAL_wcscat
#define _wcsstr     PAL_wcsstr
#else // PAL_STDCPP_COMPAT
#define _iswprint   iswprint
#define _wcslen     wcslen
#define _wcsncmp    wcsncmp
#define _wcsrchr    wcsrchr
#define _wcscmp     wcscmp
#define _wcschr     wcschr
#define _wcscspn    wcscspn
#define _wcscat     wcscat
#define _wcsstr     wcsstr
#endif // !PAL_STDCPP_COMPAT

typedef uintptr_t TADDR;
typedef ULONG64 CLRDATA_ADDRESS;

// Convert between CLRDATA_ADDRESS and TADDR.
#define TO_TADDR(cdaddr) ((TADDR)(cdaddr))
#define TO_CDADDR(taddr) ((CLRDATA_ADDRESS)(LONG_PTR)(taddr))

const int mdNameLen = 2048;
