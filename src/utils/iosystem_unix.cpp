// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file iosystem_unix.cpp  This file contains unix-specific definitions of
/// IOSystem class members (see iosystem.h).

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdexcept>
#include <algorithm>
#include "utils/logger.h"

#include "iosystem_unix.h"

namespace
{
    // short alias for full class name
    typedef netcoredbg::IOSystemTraits<netcoredbg::UnixPlatformTag> Class;
    
    
    struct AsyncRead
    {
        int    fd;
        void*  buffer;
        size_t size;

        AsyncRead(int fd, void *buf, size_t size) : fd(fd), buffer(buf), size(size) {}

        Class::IOResult operator()()
        {
            // TODO need to optimize code to left only one syscall.
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            struct timeval tv = {0, 0};
            ssize_t result = ::select(fd + 1, &set, NULL, &set, &tv);
            if (result == 0)
                return {Class::IOResult::Pending, 0};

            if (result >= 0)
                result = read(fd, buffer, size);

            if (result < 0)
            {
                if (errno == EAGAIN)
                    return {Class::IOResult::Pending, 0};

                // TODO make exception class
                char buf[1024];
                char msg[256];
                snprintf(msg, sizeof(msg), "select: %s", ErrGetStr(errno, buf, sizeof(buf)));
                throw std::runtime_error(msg);
            }

            return {result == 0 ? Class::IOResult::Eof : Class::IOResult::Success, size_t(result)};
        }

        int poll(fd_set* read, fd_set *, fd_set* except) const
        {
            FD_SET(fd, read);
            FD_SET(fd, except);
            return fd;
        }
    };

    struct AsyncWrite
    {
        int         fd;
        void const* buffer;
        size_t      size;

        AsyncWrite(int fd, const void *buf, size_t size) : fd(fd), buffer(buf), size(size) {}

        Class::IOResult operator()()
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            struct timeval tv = {0, 0};
            ssize_t result = select(fd + 1, NULL, &set, NULL, &tv);
            if (result == 0)
                return {Class::IOResult::Pending, 0};

            if (result >= 0)
                result = write(fd, buffer, size);

            if (result < 0)
            {
                if (errno == EAGAIN)
                    return {Class::IOResult::Pending, 0};

                char buf[1024];
                char msg[256];
                snprintf(msg, sizeof(msg), "select: %s", ErrGetStr(errno, buf, sizeof(buf)));
                throw std::runtime_error(msg);
            }

            return {Class::IOResult::Success, size_t(result)};
        }


        int poll(fd_set *, fd_set *write, fd_set *) const
        {
            FD_SET(fd, write);
            return fd;
        }
    };
}


template <typename T> Class::AsyncHandle::Traits  Class::AsyncHandle::TraitsImpl<T>::traits =
{
    [](void *thiz) 
        -> Class::IOResult { return reinterpret_cast<T*>(thiz)->operator()(); },

    [](void *thiz, fd_set* read, fd_set* write, fd_set* except)
        -> int { return reinterpret_cast<T*>(thiz)->poll(read, write, except); },

    [](void *src, void *dst)
        -> void { *reinterpret_cast<T*>(dst) = *reinterpret_cast<T*>(src); },

    [](void *thiz)
        -> void { reinterpret_cast<T*>(thiz)->~T(); }
};


// Function should create unnamed pipe and return two file handles
// (reading and writing pipe ends) or return empty file handles if pipe can't be created.
std::pair<Class::FileHandle, Class::FileHandle> Class::unnamed_pipe()
{
    int fds[2];
    if (::pipe(fds) < 0)
    {
        perror("pipe");
        return {};
    }

    // TODO what to do with this?
    signal(SIGPIPE, SIG_IGN);

    return { fds[0], fds[1] };
}


// Function creates listening TCP socket on given port, waits, accepts single
// connection, and return file descriptor related to the accepted connection.
// In case of error, empty file handle will be returned.
Class::FileHandle Class::listen_socket(unsigned port)
{
    assert(port > 0 && port < 65536);

    int newsockfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    int sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
    {
        perror("can't create socket");
        return {};
    }

    int enable = 1;
    if (setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        ::close(sockFd);
        perror("can't set socket options");
        return {};
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (::bind(sockFd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::close(sockFd);
        perror("can't bind to specified port");
        return {};
    }

    ::listen(sockFd, 1);

#ifdef DEBUGGER_FOR_TIZEN
    // On Tizen, launch_app won't terminate until stdin, stdout and stderr are closed.
    // But Visual Studio initiates the connection only after launch_app termination,
    // therefore we need to close the descriptors before the call to accept().
    int fd_null = open("/dev/null", O_WRONLY | O_APPEND);
    if (fd_null < 0)
    {
        ::close(sockFd);
        perror("can't open /dev/null");
        return {};
    }

    // Silently close previous stdin/stdout/stderr and reuse fds.
    // Don't allow stdin read (EOF), but allow stdout/stderr write.
    if (dup2(fd_null, STDIN_FILENO) == -1 ||
        dup2(fd_null, STDOUT_FILENO) == -1 ||
        dup2(fd_null, STDERR_FILENO) == -1)
    {
        ::close(sockFd);
        perror("can't dup2");
        return {};
    }

    close(fd_null);

    //TODO on Tizen redirect stderr/stdout output into dlog
#endif

    clilen = sizeof(cli_addr);
    newsockfd = ::accept(sockFd, (struct sockaddr *) &cli_addr, &clilen);
    ::close(sockFd);
    if (newsockfd < 0)
    {
        perror("accept");
        return {};
    }

    return newsockfd;
}

// Enable/disable handle inheritance for child processes.
Class::IOResult Class::set_inherit(const FileHandle &fh, bool inherit)
{
    int flags = fcntl(fh.fd, F_GETFD);
    if (flags < 0)
        return {IOResult::Error, 0};

    if (inherit)
        flags &= ~FD_CLOEXEC;
    else
        flags |= FD_CLOEXEC;

    if (fcntl(fh.fd, F_SETFD, flags) < 0)
        return {IOResult::Error, 0};

    return {IOResult::Success, 0};
}

// Function perform reading from the file: it may read up to `count' bytes to `buf'.
Class::IOResult Class::read(const FileHandle &fh, void *buf, size_t count)
{
    ssize_t rsize = ::read(fh.fd, buf, count);
    if (rsize < 0)
        return { (errno == EAGAIN ? IOResult::Pending : IOResult::Error), 0 };
    else
        return { (rsize == 0 ? IOResult::Eof : IOResult::Success), size_t(rsize) };
}


// Function perform writing to the file: it may write up to `count' byte from `buf'.
Class::IOResult Class::write(const FileHandle &fh, const void *buf, size_t count)
{
    ssize_t wsize = ::write(fh.fd, buf, count);
    if (wsize < 0)
        return { (errno == EAGAIN ? IOResult::Pending : IOResult::Error), 0 };
    else
        return { IOResult::Success, size_t(wsize) };
}


Class::AsyncHandle Class::async_read(const FileHandle& fh, void *buf, size_t count)
{
    return fh.fd == -1 ? AsyncHandle() : AsyncHandle::create<AsyncRead>(fh.fd, buf, count);
}

Class::AsyncHandle Class::async_write(const FileHandle& fh, const void *buf, size_t count)
{
    return fh.fd == -1 ? AsyncHandle() : AsyncHandle::create<AsyncWrite>(fh.fd, buf, count);
}


bool Class::async_wait(IOSystem::AsyncHandleIterator begin, IOSystem::AsyncHandleIterator end, std::chrono::milliseconds timeout)
{
    fd_set read_set, write_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    int maxfd = -1;
    for (IOSystem::AsyncHandleIterator it = begin; it != end; ++it)
    {
        if (*it)
            maxfd = std::max(it->handle.poll(&read_set, &write_set, &except_set), maxfd);
    }

    struct timeval tv;
    std::chrono::microseconds us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    tv.tv_sec = us.count() / 1000000, tv.tv_usec = us.count() % 1000000;

    int result;
    do result = ::select(maxfd + 1, &read_set, &write_set, &except_set, &tv);
    while (result < 0 && errno == EINTR);

    if (result < 0)
    {
        char buf[1024];
        char msg[256];
        snprintf(msg, sizeof(msg), "select: %s", ErrGetStr(errno, buf, sizeof(buf)));
        throw std::runtime_error(msg);
    }

    return result > 0;
}

Class::IOResult Class::async_cancel(Class::AsyncHandle& handle)
{
    if (!handle)
        return {Class::IOResult::Error, 0};

    handle = {};
    return {Class::IOResult::Success, 0};
}

Class::IOResult Class::async_result(Class::AsyncHandle& handle)
{
    if (!handle)
        return {Class::IOResult::Error, 0};

    auto result = handle();
    if (result.status != Class::IOResult::Pending)
        handle = {};

    return result;
}


// Function closes the file represented by file handle.
Class::IOResult Class::close(const FileHandle &fh)
{
    return { (::close(fh.fd) == 0 ? IOResult::Success : IOResult::Error), 0 };
}


// This function returns triplet of currently selected standard files.
netcoredbg::IOSystem::StdFiles Class::get_std_files()
{
    static const IOSystem::FileHandle handles[std::tuple_size<IOSystem::StdFiles>::value] = {
        FileHandle(STDIN_FILENO), FileHandle(STDOUT_FILENO), FileHandle(STDERR_FILENO)
    };
    return {handles[0], handles[1], handles[2]};
}


// StdIOSwap class allows to substitute set of standard IO files with one provided to constructor.
// Substitution exists only during life time of StsIOSwap instance.
Class::StdIOSwap::StdIOSwap(const StdFiles& files) : m_valid(true)
{
    const static unsigned NFD = std::tuple_size<StdFiles>::value;
    static const int oldfd[NFD] = { StdFileType::Stdin, StdFileType::Stdout, StdFileType::Stderr };

    const int newfd[NFD] = {
        std::get<StdFileType::Stdin>(files).handle.fd,
        std::get<StdFileType::Stdout>(files).handle.fd,
        std::get<StdFileType::Stderr>(files).handle.fd };

    for (unsigned n = 0; n < NFD; n++)
    {
        m_orig_fd[n] = ::dup(oldfd[n]);
        if (m_orig_fd[n] == -1)
        {
            char buf[1024];
            char msg[256];
            snprintf(msg, sizeof(msg), "dup(%d): %s", oldfd[n], ErrGetStr(errno, buf, sizeof(buf)));
            throw std::runtime_error(msg);
        }

        if (::dup2(newfd[n], oldfd[n]) == -1)
        {
            char buf[1024];
            char msg[256];
            snprintf(msg, sizeof(msg), "dup2(%d, %d): %s", newfd[n], oldfd[n], ErrGetStr(errno, buf, sizeof(buf)));
            throw std::runtime_error(msg);
        }
    }
}


Class::StdIOSwap::~StdIOSwap()
{
    if (!m_valid)
        return;

    const static unsigned NFD = std::tuple_size<StdFiles>::value;
    static const int oldfd[NFD] = { StdFileType::Stdin, StdFileType::Stdout, StdFileType::Stderr };
    for (unsigned n = 0; n < NFD; n++)
    {
        if (::dup2(m_orig_fd[n], oldfd[n]) == -1)
        {
            abort();
        }
        
        ::close(m_orig_fd[n]);
    }
}

#endif  // __unix__
