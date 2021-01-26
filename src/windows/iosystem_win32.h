// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file iosystem_win32.h  This file contains windows-specific declaration of IOSystem class (see iosystem.h).
//
#ifdef _WIN32
#pragma once
#include <winsock2.h>
#include <windows.h> // TODO
#include <assert.h>
#include <tuple>

namespace netcoredbg
{

template <> struct IOSystemTraits<Win32PlatformTag>
{
    struct FileHandle
    {
        FileHandle() : handle(INVALID_HANDLE_VALUE), type(FileOrPipe) {}

        explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }

        enum FileType
        {
            FileOrPipe,
            Socket
        };

        FileHandle(HANDLE filefd) : handle(filefd), type(FileOrPipe) {}
        FileHandle(SOCKET sockfd) : handle((HANDLE)sockfd), type(Socket) {}

        HANDLE handle;
        enum FileType type;
    };

    struct AsyncHandle
    {
        HANDLE handle;
        std::unique_ptr<OVERLAPPED> overlapped;

        AsyncHandle() : handle(INVALID_HANDLE_VALUE), overlapped() {}

        AsyncHandle(AsyncHandle&& other) noexcept : handle(other.handle), overlapped(std::move(other.overlapped))
        {
            other.handle = INVALID_HANDLE_VALUE;
        }

        AsyncHandle& operator=(AsyncHandle&& other) noexcept
        {
            return this->~AsyncHandle(), *new (this) AsyncHandle(std::move(other));
        }

        explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
    };

    using IOSystem = typename IOSystemImpl<IOSystemTraits<Win32PlatformTag> >;
    using IOResult = IOSystem::IOResult;

    static std::pair<FileHandle, FileHandle> unnamed_pipe();
    static FileHandle listen_socket(unsigned tcp_port);
    static IOResult set_inherit(const FileHandle &, bool);
    static IOResult read(const FileHandle &, void *buf, size_t count);
    static IOResult write(const FileHandle &, const void *buf, size_t count);
    static AsyncHandle async_read(const FileHandle &, void *buf, size_t count);
    static AsyncHandle async_write(const FileHandle &, const void *buf, size_t count);
    static bool async_wait(IOSystem::AsyncHandleIterator begin, IOSystem::AsyncHandleIterator end, std::chrono::milliseconds);
    static IOResult async_cancel(AsyncHandle &);
    static IOResult async_result(AsyncHandle &);
    static IOResult close(const FileHandle &);

    static IOSystem::StdFiles get_std_files();

    struct StdIOSwap
    {
        using StdFiles = IOSystem::StdFiles;
        using StdFileType = IOSystem::StdFileType;
        StdIOSwap(const StdFiles &);
        ~StdIOSwap();

        HANDLE m_orig_handle[std::tuple_size<StdFiles>::value];
        int m_orig_fd[std::tuple_size<StdFiles>::value];
    };
};

} // ::netcoredbg
#endif // WIN32
