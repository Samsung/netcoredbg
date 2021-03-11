// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file iosystem_unix.h  This file contains unix-specific declaration of IOSystem class (see iosystem.h).

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#pragma once
#include <cstdlib>
#include <cassert>
#include <sys/select.h>
#include <tuple>
#include <new>

#include "platform.h"
#include "iosystem.h"

template <> struct netcoredbg::IOSystemTraits<netcoredbg::UnixPlatformTag>
{
    using IOSystem = typename netcoredbg::IOSystemImpl<IOSystemTraits<UnixPlatformTag> >;
    using IOResult = IOSystem::IOResult;

    struct FileHandle
    {
        FileHandle() : fd(-1) {}
        FileHandle(int n) : fd(n) {}
        explicit operator bool() const { return fd != -1; }

        int fd;
    };

    struct AsyncHandle
    {
        struct Traits
        {
            IOResult (*oper)(void *thiz);
            int (*poll)(void *thiz, fd_set *, fd_set *, fd_set *);
            void (*move)(void* src, void *dst);
            void (*destr)(void *thiz);
        };
        
        template <typename T> struct TraitsImpl
        {
            static struct Traits traits;
        };

        const Traits *traits;
        mutable char data alignas(__BIGGEST_ALIGNMENT__) [sizeof(void*) * 4];

        explicit operator bool() const { return !!traits; }

        IOResult operator()() { assert(*this); return traits->oper(data); }

        int poll(fd_set* read, fd_set* write, fd_set* except)
        {
            assert(*this);
            return traits->poll(data, read, write, except);
        }

        AsyncHandle() : traits(nullptr) {}

        template <typename InstanceType, typename... Args>
        static AsyncHandle create(Args&&... args)
        {
            static_assert(sizeof(InstanceType) <= sizeof(data), "insufficiend data size");
            AsyncHandle result;
            result.traits = &TraitsImpl<InstanceType>::traits;
            new (result.data) InstanceType(std::forward<Args>(args)...);
            return result;
        }

        AsyncHandle(AsyncHandle&& other) : traits(other.traits)
        {
            if (other) traits->move(other.data, data);
            other.traits = nullptr;
        }

        AsyncHandle& operator=(AsyncHandle&& other)
        {
            this->~AsyncHandle();
            return *new (this) AsyncHandle(std::move(other));
        }

        ~AsyncHandle() { if (*this) traits->destr(data); }
    };

    static std::pair<FileHandle, FileHandle> unnamed_pipe();
    static FileHandle listen_socket(unsigned tcp_port);
    static IOResult set_inherit(const FileHandle&, bool);
    static IOResult read(const FileHandle&, void *buf, size_t count);
    static IOResult write(const FileHandle&, const void *buf, size_t count);
    static AsyncHandle async_read(const FileHandle&, void *buf, size_t count);
    static AsyncHandle async_write(const FileHandle&, void *buf, size_t count);
    static bool async_wait(IOSystem::AsyncHandleIterator begin, IOSystem::AsyncHandleIterator end, std::chrono::milliseconds);
    static IOResult async_cancel(AsyncHandle&);
    static IOResult async_result(AsyncHandle&);
    static IOResult close(const FileHandle&);

    struct StdIOSwap
    {
        using StdFiles = IOSystem::StdFiles;
        using StdFileType = IOSystem::StdFileType;
        StdIOSwap(const StdFiles &);
        ~StdIOSwap();

        int m_orig_fd[std::tuple_size<StdFiles>::value];
    };

    static IOSystem::StdFiles get_std_files();
};

#endif // __unix__
