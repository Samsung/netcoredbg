// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file iosystem.h  This file contains declaration of IOSystem class, which provices
/// cross-platform interface to files, pipes or sockets...

#pragma once
#include <cstddef>
#include <utility>
#include <type_traits>
#include <chrono>

#include "utils/platform.h"

namespace netcoredbg
{

/// This is platform specific class, which contains implementation details for particular platform.
template <typename PlatformTag> struct IOSystemTraits;

/// This is definition of IOSystem class (which should be parametrized with IOSystemTraits for particular platform).
template <typename Traits> struct IOSystemImpl
{
    /// Structure represents result of read or write operation:
    struct IOResult
    {
        enum Status 
        {
            Success,    /// operaion was completed successfully, see `size' field.
            Error,      /// IO error, reading side of socket or pipe was closed (when writing)
            Eof,        /// end of file reached, writing side of socket or pipe was closed (when reading)
            Pending     /// operation can't be completed in non-blocked mode (will block)
        };
        
        Status status;  /// operation result
        size_t size;    /// amount of written/read data in bytes
    };


    /// Handle of asynchronous operation, for which result can be requested via call to
    /// `async_result` or operation can be canceled via call to `async_cancel`. This
    /// handle is returned by `async_read` or `async_write` functions.
    struct AsyncHandle // TODO solve issue with inheritance
    {
        explicit operator bool() const { return !!handle; }
        
    //private:
        //friend struct IOSystemImpl;
        typename Traits::AsyncHandle handle;
    };

    /// Iterator type, which is passed to `async_wait` function to specify list of
    /// asynchonouse operation handles for which waiting is performed.
    /// Actually this is type erasing iterator of InputIterator class.
    struct AsyncHandleIterator
    {
    private:
        struct Operations
        {
            void (*next)(void *);
            AsyncHandle& (*get)(void *);
            void (*destr)(void *);
        };

        template <typename T> struct OpsImpl { static const Operations ops; };

        constexpr static const size_t MaxIteratorSize = sizeof(void*) * 2;

        const Operations& ops;

        // Note, we can't use `alignas(alignof(std::max_align_t))` here, since at least MSVC 32bit (VS2019) compiler can't
        // generate proper code and ASAN detect "ERROR: AddressSanitizer: stack - buffer - underflow on address...".
        union data_align_t
        {
            std::max_align_t align_field;
            mutable char data[MaxIteratorSize];
        };
        data_align_t data_align_tmp;
        char * const data;

    public:
        /// Iterator might be created from any other iterator or pointer type,
        /// for which `operator*` returns reference to `AsyncHandle`.
        template <typename Iterator,
            typename = typename std::enable_if<std::is_convertible<decltype(*std::declval<Iterator>()), AsyncHandle&>::value>::type>
        AsyncHandleIterator(Iterator it) : ops(OpsImpl<Iterator>::ops), data(data_align_tmp.data) { new (data) Iterator(it); }

        ~AsyncHandleIterator() { ops.destr(data); }

        /// @{ Access to `AsyncHandle` values.
        AsyncHandle& operator*() const { return ops.get(data); }
        AsyncHandle* operator->() const { return &**this; }
        /// @}

        /// @{ Iterators might be directly compared.
        bool operator==(const AsyncHandleIterator& other) const { return &**this == &*other; }
        bool operator!=(const AsyncHandleIterator& other) const { return !(*this == other); }
        /// @}

        /// Advance to next `AsyncHandle` value.
        AsyncHandleIterator& operator++() { return ops.next(data), *this; }
    };


    class StdIOSwap;    // forward declaration

    /// FileHandle data type is opaque structure, which represents opened file handle.
    /// File handles not represents files itself, file handle is just like reference, so
    /// file handles can be freely copied, deleted, etc... this not affect opened files.
    /// File handles implement operator bool() which evaluates to false, if file handle
    /// is "empty" and not represents any opened files.
    struct FileHandle
    {
        /// FileHandle is default-constructible and initially is empty.
        FileHandle() : handle() {}

        /// FileHandle can be checked, if it is empty or not.
        explicit operator bool() const { return !!handle; }

    // private:
        // friend struct IOSystemImpl<Traits>;
        // friend class IOSystemImpl<Traits>::StdIOSwap;   // https://stackoverflow.com/questions/30418270/clang-bug-namespaced-template-class-friend   FIXME WHY THIS NOT WORKS?

        FileHandle(typename Traits::FileHandle h) : handle(h) {}

        typename Traits::FileHandle handle;
    };

    /// Function should create unnamed pipe and return two file handles
    /// (reading and writing pipe ends) or return empty file handles if pipe can't be created.
    static std::pair<FileHandle, FileHandle> unnamed_pipe()
    {
        auto pipe = Traits::unnamed_pipe();
        return {pipe.first, pipe.second};
    }

    /// Function creates listening TCP socket on given port, waits, accepts single
    /// connection, and return file descriptor related to the accepted connection.
    /// In case of error, empty file handle will be returned.
    static FileHandle listen_socket(unsigned tcp_port) { return Traits::listen_socket(tcp_port); }

    /// Function perform reading from the file: it may read up to `count' bytes to `buf'.
    static IOResult read(FileHandle fh, void *buf, size_t count) { return Traits::read(fh.handle, buf, count); }

    /// Function perform writing to the file: it may write up to `count' byte from `buf'.
    static IOResult write(FileHandle fh, const void *buf, size_t count) { return Traits::write(fh.handle, buf, count); }

    /// Enable or disable handle inheritance for child processes.
    static IOResult set_inherit(FileHandle fh, bool inherit_handle) { return Traits::set_inherit(fh.handle, inherit_handle); }
    
    /// Start asynchronous read operation: request to read `count` bytes to the
    /// buffer `buf`, from file handle `fh`. Function returns `AsyncHandle` value,
    /// for which later result must be obtained via call to `async_result` or
    /// operation must be canceled via call to `async_cancel`.
    static AsyncHandle async_read(FileHandle fh, void *buf, size_t count) { return {Traits::async_read(fh.handle, buf, count)}; }

    /// Start asynchronous write operation: request to write `count` bytes from the
    /// buffer `buf`, to file handle `fh`. Function returns `AsyncHandle` value,
    /// for which later result must be obtained via call to `async_result` or
    /// operation must be canceled via call to `async_cancel`.
    static AsyncHandle async_write(FileHandle fh, const void *buf, size_t count) { return {Traits::async_write(fh.handle, buf, count)}; }

    /// This function allows to wait until one of the specified asynchronous operations
    /// is finished, or until timeout expired. Function returns `true` if at least one
    /// asynchronous operation is finished.
    static bool async_wait(AsyncHandleIterator begin, AsyncHandleIterator end, std::chrono::milliseconds timeout)
    {
        return Traits::async_wait(begin, end, timeout);
    }

    /// Function cancels previously started asynchronous operation.
    static IOResult async_cancel(AsyncHandle& h) { return {Traits::async_cancel(h.handle)}; }

    /// Function checks result of the synchronous operation. Result status might have
    /// one of the following values:
    ///    * Success -- operation completed successfully (`AsyncHandle` now is invalid/deleted);
    ///    * Pending -- operation in progress (you still need call `async_result` or `async_cancel` again!);
    ///    * Error -- operation completed with an error (`AsyncHandle` now is invalid/deleted).
    static IOResult async_result(AsyncHandle& h) { return {Traits::async_result(h.handle)}; }

    /// Function closes the file represented by file handle.
    static IOResult close(FileHandle fh) { return Traits::close(fh.handle); }
    
    /// This enumeration represents standard IO files.
    enum StdFileType
    {
        Stdin = 0,   // Note: numbers here is fixed and shouldn't be changed. 
        Stdout = 1,
        Stderr = 2
    };

    /// Data type contaning file handles for all standard files.
    typedef std::tuple<const FileHandle&, const FileHandle&, const FileHandle&> StdFiles;

    /// This function returns triplet of currently selected standard files.
    static StdFiles get_std_files() { return Traits::get_std_files(); }

    /// This class allows to substitute set of standard IO files with one provided to constructor.
    /// Substitution exists only during life time of StsIOSwap instance.
    class StdIOSwap
    {
        typename Traits::StdIOSwap ioswap;

    public:
        StdIOSwap(const StdFiles &files) : ioswap(files) {}

        StdIOSwap(const StdIOSwap&) = delete;
    };

}; // struct IOSystemImpl


template <typename Traits> template <typename IteratorType>
const typename IOSystemImpl<Traits>::AsyncHandleIterator::Operations
IOSystemImpl<Traits>::AsyncHandleIterator::OpsImpl<IteratorType>::ops =
{
    [](void *thiz) { ++*reinterpret_cast<IteratorType*>(thiz); },
    [](void *thiz) -> typename IOSystemImpl<Traits>::AsyncHandle& { return **reinterpret_cast<IteratorType*>(thiz); },
    [](void *thiz) { reinterpret_cast<IteratorType*>(thiz)->~IteratorType(); }
};

} // ::netcoredbg


// include here all platform specific implementations of IOSystemTraits<T> classes
#include "iosystem_unix.h"
#include "iosystem_win32.h"

namespace netcoredbg
{
    /// IOSystem class represents IOSystemImpl<T> for current platform.
    typedef IOSystemImpl<IOSystemTraits<PlatformTag> > IOSystem;
}
