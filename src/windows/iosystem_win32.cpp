// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file iosystem_win32.cpp  This file contains windows-specific definitions of
/// IOSystem class members (see iosystem.h).

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <stdexcept>
#include <new>
#include <memory>
#include <atomic>
#include <string.h>
#include <assert.h>
#include "utils/iosystem.h"
#include "utils/limits.h"

// short alias for full class name
namespace { typedef netcoredbg::IOSystemTraits<netcoredbg::Win32PlatformTag> Class; }

namespace
{
    class Win32Exception : public std::runtime_error
    {
        struct Msg
        {
            mutable char buf[2 * LINE_MAX];
        };

        static const char* getmsg(const char *prefix, DWORD error, const Msg& msg = Msg())
        {
            int len = prefix ? snprintf(msg.buf, sizeof(msg.buf), "%s: ", prefix) : 0;

            if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), 
                    msg.buf + len, sizeof(msg.buf) - len, NULL))
            {
                return msg.buf;
            }
            
            snprintf(msg.buf + len, sizeof(msg.buf) - len, "error %#x", error);
            return msg.buf;
        }

    public:
        /// Specify Win32 error code and, optionally, error message prefix.
        Win32Exception(DWORD error, const char* prefix = nullptr) : std::runtime_error(getmsg(prefix, error)) {}

        /// Specify error message prefix (optionally). Win32 error code will be obtained via call to GetLastError().
        Win32Exception(const char *prefix = nullptr) : Win32Exception(prefix, GetLastError()) {}

        /// Specify explicitly error message prefix and error code.
        Win32Exception(const char *prefix, DWORD error) : Win32Exception(error, prefix) {}
    };

    struct Initializer
    {
        Initializer()
        {
            WSADATA wsa;
            int wsa_error = WSAStartup(MAKEWORD(2, 2), &wsa);
            if (wsa_error != 0)
                throw Win32Exception("WSAStartup failed", wsa_error);
        }

        ~Initializer()
        {
            WSACleanup();
        }
    };

    static Initializer initializer;

#if 0 
    // assuming domain=AF_UNIX, type=SOCK_STREAM, protocol=0
    int wsa_socketpair(int domain, int type, int protocol, SOCKET sv[2])
    {

        SOCKET serv = ::socket(domain, type, protocol);
        if (serv == INVALID_SOCKET)
            throw Win32Exception("can't create socket", WSAGetLastError());

        // TODO
        char name[] = "netcoredbg";
        size_t namelen = sizeof(name)-1;

        SOCKADDR_UN sa;
        sa.sun_family = domain;
        assert(namelen <= sizeof(sa.sun_path));
        memcpy(sa.sun_path, name, namelen); 
        if (::bind(serv, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            throw Win32Exception("can't bind socket", err);
        }

        u_long mode = 1;
        if (::ioctlsocket(serv, FIONBIO, &mode) == SOCKET_ERROR)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            throw Win32Exception("ioctlsocket(FIONBIO)", err);
        }

        if (::listen(serv, 1) == SOCKET_ERROR && WSAGetLastError() != WSAEINPROGRESS)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            throw Win32Exception("ioctlsocket(FIONBIO)", err);
        }

        SOCKET conn = ::socket(domain, type, protocol);
        if (conn == INVALID_SOCKET)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            throw Win32Exception("can't create socket", err);
        }

        sa.sun_family = domain;
        memcpy(sa.sun_path, name, namelen);
        if (::connect(conn, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            ::closesocket(conn);
            throw Win32Exception("can't bind socket", err);
        }

        mode = 0;
        if (::ioctlsocket(serv, FIONBIO, &mode) == SOCKET_ERROR)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            ::closesocket(conn);
            throw Win32Exception("ioctlsocket(FIONBIO)", err);
        }

        SOCKET newsock = ::accept(serv, NULL, NULL);
        if (newsock == INVALID_SOCKET)
        {
            auto err = WSAGetLastError();
            ::closesocket(serv);
            ::closesocket(conn);
            throw Win32Exception("accept on socket", err);
        }

        ::closesocket(serv);

        sv[0] = newsock, sv[1] = conn;
        return 0;
    }
#endif
}


// Function should create unnamed pipe and return two file handles
// (reading and writing pipe ends) or return empty file handles if pipe can't be created.
std::pair<Class::FileHandle, Class::FileHandle> Class::unnamed_pipe()
{
#if 0
    SOCKET sv[2];
    if (wsa_socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return {FileHandle(), FileHandle()};
#endif

    static const size_t PipeSize = 32 * LINE_MAX;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE reading_fd, writing_fd;

    static std::atomic<long> pipe_num;
    char pipe_name[MAX_PATH + 1];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\Pipe\\Win32Pipes.%08x.%08x",
                GetCurrentProcessId(), pipe_num++);

    reading_fd = CreateNamedPipeA(pipe_name,
                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_WAIT,
                    1,  // number of pipes
                    PipeSize, PipeSize,
                    0,  // 50ms default timeout
                    &saAttr);
                
    if (reading_fd == INVALID_HANDLE_VALUE)
    {
        perror("CreateNamedPipeA");
        return { FileHandle(), FileHandle() };
    }

    writing_fd = CreateFileA(pipe_name,
                    GENERIC_WRITE,
                    0,  // no sharing
                    &saAttr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                    NULL);

    if (writing_fd == INVALID_HANDLE_VALUE)
    {
        auto err = GetLastError();
        ::CloseHandle(writing_fd);
        fprintf(stderr, "CreateFile pipe error: %#x\n", err);
        return { FileHandle(), FileHandle() };
    }

    if (!SetHandleInformation(writing_fd, HANDLE_FLAG_INHERIT, 0))
    {
        fprintf(stderr, "SetHandleInformation failed!\n");
        return { FileHandle(), FileHandle() };
    }

    if (!SetHandleInformation(reading_fd, HANDLE_FLAG_INHERIT, 0))
    {
        fprintf(stderr, "SetHandleInformation failed!\n");
        return { FileHandle(), FileHandle() };
    }

    return { FileHandle(reading_fd), FileHandle(writing_fd) };
}


// Function creates listening TCP socket on given port, waits, accepts single
// connection, and return file descriptor related to the accepted connection.
// In case of error, empty file handle will be returned.
Class::FileHandle Class::listen_socket(unsigned port)
{
    assert(port > 0 && port < 65536);

    SOCKET newsockfd;
    int clilen;
    struct sockaddr_in serv_addr, cli_addr;

    SOCKET sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd == INVALID_SOCKET)
    {
        fprintf(stderr, "can't create socket: %#x\n", WSAGetLastError());
        return {};
    }

    BOOL enable = 1;
    if (::setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enable, sizeof(BOOL)) == SOCKET_ERROR)
    {
        ::closesocket(sockFd);
        fprintf(stderr, "setsockopt failed\n");
        return {};
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (::bind(sockFd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR)
    {
        ::closesocket(sockFd);
        fprintf(stderr, "can't bind to specified port!\n");
        return {};
    }

    ::listen(sockFd, 1);

    clilen = sizeof(cli_addr);
    newsockfd = ::accept(sockFd, (struct sockaddr*)&cli_addr, &clilen);
    ::closesocket(sockFd);
    if (newsockfd == INVALID_SOCKET)
    {
        fprintf(stderr, "can't accept connection\n");
        return {};
    }

    return FileHandle(newsockfd);
}

// Function enables or disables inheritance of file handle for child processes.
Class::IOResult Class::set_inherit(const FileHandle& fh, bool inherit)
{
    DWORD flags;
    if (!GetHandleInformation(fh.handle, &flags))
        return {IOResult::Error};

    if (inherit)
        flags |= HANDLE_FLAG_INHERIT;
    else
        flags &= ~HANDLE_FLAG_INHERIT;

    if (!SetHandleInformation(fh.handle, HANDLE_FLAG_INHERIT, flags))
        return {IOResult::Error};

    return {IOResult::Success};
}

// Function perform reading from the file: it may read up to `count' bytes to `buf'.
Class::IOResult Class::read(const FileHandle& fh, void *buf, size_t count)
{
    DWORD dwRead = 0;
    OVERLAPPED ov = {};
    if (! ReadFile(fh.handle, buf, (DWORD)count, &dwRead, &ov))
        return { (GetLastError() == ERROR_IO_PENDING ? IOResult::Pending : IOResult::Error), dwRead };
    else
        return { (dwRead == 0 ? IOResult::Eof : IOResult::Success), dwRead };
}


// Function perform writing to the file: it may write up to `count' byte from `buf'.
Class::IOResult Class::write(const FileHandle& fh, const void *buf, size_t count)
{
    // see https://stackoverflow.com/questions/43939424/writefile-with-windows-sockets-returns-invalid-parameter-error
    DWORD dwWritten = 0;
    OVERLAPPED ov = {};
    if (! WriteFile(fh.handle, buf, (DWORD)count, &dwWritten, &ov))
        return { (GetLastError() == ERROR_IO_PENDING ? IOResult::Pending : IOResult::Error), dwWritten };
    else
        return { IOResult::Success, dwWritten };
}


Class::AsyncHandle Class::async_read(const FileHandle& fh, void *buf, size_t count)
{
    if (fh.handle == INVALID_HANDLE_VALUE)
        return {};

    AsyncHandle result; 
    result.check_eof = true;
    result.handle = fh.handle;
    result.overlapped.reset(new OVERLAPPED);
    memset(result.overlapped.get(), 0, sizeof(OVERLAPPED));
    result.overlapped->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (result.overlapped->hEvent == INVALID_HANDLE_VALUE)
        return {};

    DWORD val;
    DWORD bytesRead;

    if (GetConsoleMode(fh.handle, &val))
    {   // file handle is the console
        // first, remove all events before the first key event, if exists
        while (GetNumberOfConsoleInputEvents(fh.handle, &val) && val)
        {
            INPUT_RECORD event;
            if (!PeekConsoleInput(fh.handle, &event, 1, &bytesRead))
                return {};
            if (event.EventType != KEY_EVENT || (event.EventType == KEY_EVENT && !event.Event.KeyEvent.bKeyDown))
            {
                if (!ReadConsoleInput(fh.handle, &event, 1, &bytesRead))
                    return {};
            }
            else
                break;
        }
        if (!val)
        {
            // nothing to read from the console -- defer call to ReadFile
            result.buf = buf, result.count = count;
            return result;
        }
    }

    if (! ReadFile(fh.handle, buf, (DWORD)count, nullptr, result.overlapped.get()))
    {
        if (GetLastError() != ERROR_IO_PENDING)
            return {};
    }

    return result;
}

Class::AsyncHandle Class::async_write(const FileHandle& fh, const void *buf, size_t count)
{
    if (fh.handle == INVALID_HANDLE_VALUE)
        return {};

    AsyncHandle result;
    result.handle = fh.handle;
    result.overlapped.reset(new OVERLAPPED);
    memset(result.overlapped.get(), 0, sizeof(OVERLAPPED));
    result.overlapped->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (result.overlapped->hEvent == INVALID_HANDLE_VALUE)
        return {};

    if (! WriteFile(fh.handle, buf, (DWORD)count, nullptr, result.overlapped.get()))
    {
        if (GetLastError() != ERROR_IO_PENDING)
            return {};
    }

    return result;
}

bool Class::async_wait(IOSystem::AsyncHandleIterator begin, IOSystem::AsyncHandleIterator end, std::chrono::milliseconds timeout)
{
    // console workaround
    for (auto it = begin; it != end; ++it)
    {
        if (it->handle.buf)
        {
            DWORD val;
            if (GetNumberOfConsoleInputEvents(it->handle.handle, &val) && val)
                SetEvent(it->handle.overlapped->hEvent);
        }
    }

    // count number of active handles
    unsigned count = 0;
    for (auto it = begin; it != end; ++it)
        if (*it) ++count;

    // allocate memory for events array
    HANDLE *events = static_cast<HANDLE*>(alloca(count * sizeof(HANDLE)));
    unsigned n = 0;
    for (auto it = begin; it != end; ++it)
    {
        if (*it)
            events[n++] = it->handle.overlapped->hEvent;
    }

    assert(n == count);
    DWORD result = WaitForMultipleObjects(count, events, FALSE, DWORD(timeout.count()));
    return result != WAIT_FAILED && result != WAIT_TIMEOUT;
}

Class::IOResult Class::async_cancel(AsyncHandle& h)
{
    if (!h)
        return {IOResult::Error};
    
    if (!CloseHandle(h.overlapped->hEvent))
        perror("CloseHandle(event) error");

    // console workaround -- canceling deffered operation
    if (h.buf)
    {
        h = AsyncHandle();
        return {IOResult::Success};
    }

    IOResult result;
    if (!CancelIoEx(h.handle, h.overlapped.get()))
        result = {IOResult::Error};
    else 
        result = {IOResult::Success};

    h = AsyncHandle();
    return result;
}

Class::IOResult Class::async_result(AsyncHandle& h)
{
    if (!h)
        return {IOResult::Error};

    DWORD bytes;
    bool finished;

    if (h.buf)
    {
        // workaround for the console
        finished = ReadFile(h.handle, h.buf, DWORD(h.count), &bytes, nullptr);
    }
    else
    {
        // pipes, normal files, etc...
        finished = GetOverlappedResult(h.handle, h.overlapped.get(), &bytes, FALSE);
        if (!finished)
        {
            DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE)
                return {IOResult::Pending};
        }
    }

    if (!CloseHandle(h.overlapped->hEvent))
        perror("CloseHandle(event) error");

    bool check_eof = h.check_eof;

    h = AsyncHandle();

    if (!finished)
        return {IOResult::Error};

    if (check_eof && bytes == 0)
        return {IOResult::Eof, bytes};
        
    return {IOResult::Success, bytes};
}

// Function closes the file represented by file handle.
Class::IOResult Class::close(const FileHandle& fh)
{
    assert(fh);
    if (fh.type == FileHandle::Socket)
        return { ::closesocket((SOCKET)fh.handle) == 0 ? IOResult::Success : IOResult::Error };
    else
        return { ::CloseHandle(fh.handle) ? IOResult::Success : IOResult::Error };
}


// Function allows non-blocking IO on files, it is similar with select(2) system call on Unix.
// Arguments includes: pointers to three sets of file handles (for reading, for writing, and for
// exceptions), and timeout value, in milliseconds. Any pointer might have NULL value if some set
// isn't specified.
// Function returns -1 on error, 0 on timeout or number of ready to read/write file handles.
// If function returns value greater than zero, at least one of the sets, passed in arguments,
// is not empty and contains file handles ready to read/write/etc...


// This function returns triplet of currently selected standard files.
Class::IOSystem::StdFiles Class::get_std_files()
{
    using Handles = std::tuple<IOSystem::FileHandle, IOSystem::FileHandle, IOSystem::FileHandle>;
    /*thread_local*/ static alignas(alignof(Handles)) char mem[sizeof(Handles)];  // TODO
    Handles& handles = *new (mem) Handles {
        FileHandle(GetStdHandle(STD_INPUT_HANDLE)),
        FileHandle(GetStdHandle(STD_OUTPUT_HANDLE)),
        FileHandle(GetStdHandle(STD_ERROR_HANDLE))
    };
    return { std::get<IOSystem::Stdin>(handles),
             std::get<IOSystem::Stdout>(handles),
             std::get<IOSystem::Stderr>(handles) };
}


// StdIOSwap class allows to substitute set of standard IO files with one provided to constructor.
// Substitution exists only during life time of StsIOSwap instance.
Class::StdIOSwap::StdIOSwap(const StdFiles& files) : m_valid(true)
{
    const static unsigned NFD = std::tuple_size<StdFiles>::value;
    static const DWORD std_handles[NFD] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    static const int open_flags[NFD] = {_O_RDONLY | _O_BINARY, _O_BINARY, _O_BINARY};
    const int open_fds[NFD] = {_fileno(stdin), _fileno(stdout), _fileno(stderr)};

    const FileHandle new_handles[NFD] = {
        std::get<IOSystem::Stdin>(files).handle,
        std::get<IOSystem::Stdout>(files).handle,
        std::get<IOSystem::Stderr>(files).handle };

    fflush(stdout);
    fflush(stderr);

    for (unsigned n = 0; n < NFD; n++)
    {
        if (new_handles[n].type != FileHandle::FileOrPipe)
            throw std::runtime_error("can't use socket handle for stdin/stdout/stderr");
    }

    for (unsigned n = 0; n < NFD; n++)
    {
        m_orig_handle[n] = GetStdHandle(std_handles[n]);
        if (m_orig_handle[n] == INVALID_HANDLE_VALUE)
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "GetStdHandle(%#x): error", std_handles[n]);
            throw std::runtime_error(msg);
        }

        if (!SetHandleInformation(new_handles[n].handle, HANDLE_FLAG_INHERIT, 1))
            fprintf(stderr, "SetHandleInformation failed!\n");

        if (!SetStdHandle(std_handles[n], new_handles[n].handle))
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "SetStdHandle(%#x, %p): error", std_handles[n], new_handles[n].handle);
            throw std::runtime_error(msg);
        } 


        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(new_handles[n].handle), open_flags[n]); 
        if (fd == -1)
            throw Win32Exception("_open_osfhandle");

        m_orig_fd[n] = _dup(open_fds[n]);
        if (m_orig_fd[n] == -1)
            throw Win32Exception("_dup");

        if (_dup2(fd, open_fds[n]) == -1)
            throw Win32Exception("_dup2");

        close(fd);
    }
}

Class::StdIOSwap::~StdIOSwap()
{
    if (!m_valid)
        return;

    const static unsigned NFD = std::tuple_size<StdFiles>::value;
    static const DWORD std_handles[NFD] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    const int open_fds[NFD] = {_fileno(stdin), _fileno(stdout), _fileno(stderr)};

    fflush(stdout);
    fflush(stderr);

    for (unsigned n = 0; n < NFD; n++)
    {
        if (!SetStdHandle(std_handles[n], m_orig_handle[n]))
        {
            abort();
        }

        _dup2(m_orig_fd[n], open_fds[n]);
        close(m_orig_fd[n]);
    }
}

#endif  // WIN32
