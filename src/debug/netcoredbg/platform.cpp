// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifndef FEATURE_PAL
// Turn off macro definitions named max and min in <windows.h> header file
// to avoid compile error for std::max().
#define NOMINMAX
#endif
#ifdef _MSC_VER
// Disable compiler warning about unsafe std::copy
#define _SCL_SECURE_NO_WARNINGS
#endif

#include "platform.h"

#include <cstring>
#include <set>
#include <fstream>
#include <thread>
#include <algorithm>

#ifdef FEATURE_PAL
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <linux/limits.h>
#endif

#else
#include <windows.h>
#include <winsock2.h>
#endif

unsigned long OSPageSize()
{
    static unsigned long pageSize = 0;
#ifdef FEATURE_PAL
    if (pageSize == 0)
        pageSize = sysconf(_SC_PAGESIZE);
#endif
    return pageSize;
}

std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

// From https://stackoverflow.com/questions/13541313/handle-socket-descriptors-like-file-descriptor-fstream-c-linux
#ifdef WIN32
typedef HANDLE fd_t;
#define FD_INVALID_VALUE INVALID_HANDLE_VALUE
#else
typedef int fd_t;
#define FD_INVALID_VALUE (-1)
#endif

class fdbuf : public std::streambuf
{
private:
    enum { bufsize = 1024 };
    char outbuf_[bufsize];
    char inbuf_[bufsize + 16 - sizeof(int)];
    fd_t  fd_;
public:
    typedef std::streambuf::traits_type traits_type;

    fdbuf(fd_t fd);
    virtual ~fdbuf();
    void open(fd_t fd);
    void close();

protected:
    int overflow(int c) override;
    int underflow() override;
    int sync() override;

private:
    int fdsync();

    std::streamsize fdread(void *buf, size_t count);
    std::streamsize fdwrite(const void *buf, size_t count);
    void fdclose();
};

fdbuf::fdbuf(fd_t fd)
  : fd_(FD_INVALID_VALUE) {
    this->open(fd);
}

fdbuf::~fdbuf() {
    if (this->fd_ != FD_INVALID_VALUE) {
        this->fdsync();
        this->fdclose();
    }
}

std::streamsize fdbuf::fdread(void *buf, size_t count)
{
#ifdef WIN32
    DWORD dwRead = 0;
    BOOL bSuccess = ReadFile(this->fd_, buf, (DWORD)count, &dwRead, NULL);

    if (!bSuccess)
        dwRead = 0;

    return dwRead;
#else
    return ::read(this->fd_, buf, count);
#endif
}

std::streamsize fdbuf::fdwrite(const void *buf, size_t count)
{
#ifdef WIN32
    DWORD dwWritten = 0;
    BOOL bSuccess = WriteFile(this->fd_, buf, (DWORD)count, &dwWritten, NULL);

    if (!bSuccess)
        dwWritten = 0;

    return dwWritten;
#else
    return ::write(this->fd_, buf, count);
#endif
}

void fdbuf::fdclose()
{
#ifdef WIN32
    CloseHandle(this->fd_);
#else
    ::close(this->fd_);
#endif
}

void fdbuf::open(fd_t fd) {
    this->close();
    this->fd_ = fd;
    this->setg(this->inbuf_, this->inbuf_, this->inbuf_);
    this->setp(this->outbuf_, this->outbuf_ + bufsize - 1);
}

void fdbuf::close() {
    if (!(this->fd_ < 0)) {
        this->sync();
        this->fdclose();
    }
}

int fdbuf::overflow(int c) {
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
        *this->pptr() = traits_type::to_char_type(c);
        this->pbump(1);
    }
    return this->sync() == -1
        ? traits_type::eof()
        : traits_type::not_eof(c);
}

int fdbuf::sync() {
    return fdsync();
}

int fdbuf::fdsync() {
    if (this->pbase() != this->pptr()) {
        std::streamsize size(this->pptr() - this->pbase());
        std::streamsize done(fdwrite(this->outbuf_, size));
        // The code below assumes that it is success if the stream made
        // some progress. Depending on the needs it may be more
        // reasonable to consider it a success only if it managed to
        // write the entire buffer and, e.g., loop a couple of times
        // to try achieving this success.
        if (0 < done) {
            std::copy(this->pbase() + done, this->pptr(), this->pbase());
            this->setp(this->pbase(), this->epptr());
            this->pbump((int)(size - done));
        }
    }
    return this->pptr() != this->epptr()? 0: -1;
}

int fdbuf::underflow()
{
    if (this->gptr() == this->egptr()) {
        std::streamsize pback(std::min(this->gptr() - this->eback(),
                                       std::ptrdiff_t(16 - sizeof(int))));
        std::copy(this->egptr() - pback, this->egptr(), this->eback());
        int done((int)fdread(this->eback() + pback, bufsize));
        this->setg(this->eback(),
                   this->eback() + pback,
                   this->eback() + pback + std::max(0, done));
    }
    return this->gptr() == this->egptr()
        ? traits_type::eof()
        : traits_type::to_int_type(*this->gptr());
}

#ifdef FEATURE_PAL
class IORedirectServerHandles
{
    int m_sockFd;
    int m_clientFd;
    int m_realStdInFd;
    int m_realStdOutFd;
    int m_realStdErrFd;
    int m_appStdIn;

public:
    IORedirectServerHandles() :
        m_sockFd(-1),
        m_clientFd(-1),
        m_realStdInFd(STDIN_FILENO),
        m_realStdOutFd(STDOUT_FILENO),
        m_realStdErrFd(STDERR_FILENO),
        m_appStdIn(-1)
    {
    }

    ~IORedirectServerHandles()
    {
        if (m_sockFd == -1)
            return;
        ::close(m_clientFd);
        ::close(m_sockFd);
    }

    bool IsConnected() const { return m_clientFd != -1; }

    void RedirectOutput(
        std::function<void(std::string)> onStdOut,
        std::function<void(std::string)> onStdErr);

    bool WaitForConnection(uint16_t port);

    int GetConnectionHandle() const { return m_clientFd; }
    int GetStdIn() const { return m_realStdInFd; }
    int GetStdOut() const { return m_realStdOutFd; }
    int GetStdErr() const { return m_realStdErrFd; }
};
#else
class IORedirectServerHandles
{
    SOCKET m_sockFd;
    SOCKET m_clientFd;
    HANDLE m_realStdInFd;
    HANDLE m_realStdOutFd;
    HANDLE m_realStdErrFd;
    HANDLE m_appStdIn;

public:
    IORedirectServerHandles() :
        m_sockFd(INVALID_SOCKET),
        m_clientFd(INVALID_SOCKET),
        m_realStdInFd(GetStdHandle(STD_INPUT_HANDLE)),
        m_realStdOutFd(GetStdHandle(STD_OUTPUT_HANDLE)),
        m_realStdErrFd(GetStdHandle(STD_ERROR_HANDLE)),
        m_appStdIn(INVALID_HANDLE_VALUE)
    {
    }

    ~IORedirectServerHandles()
    {
        if (m_sockFd == INVALID_SOCKET)
            return;

        ::closesocket(m_clientFd);
        ::closesocket(m_sockFd);
        WSACleanup();
    }

    bool IsConnected() const { return m_clientFd != INVALID_SOCKET; }

    void RedirectOutput(
        std::function<void(std::string)> onStdOut,
        std::function<void(std::string)> onStdErr);

    bool WaitForConnection(uint16_t port);

    HANDLE GetConnectionHandle() const { return (HANDLE)m_clientFd; }
    HANDLE GetStdIn() const { return m_realStdInFd; }
    HANDLE GetStdOut() const { return m_realStdOutFd; }
    HANDLE GetStdErr() const { return m_realStdErrFd; }
};
#endif

#ifndef FEATURE_PAL

void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string& tpaList)
{
    const char * const tpaExtensions[] = {
        "*.ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
        "*.dll",
        "*.ni.exe",
        "*.exe",
    };

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        size_t extLength = strlen(ext);

        std::string assemblyPath(directory);
        assemblyPath.append("\\");
        assemblyPath.append(tpaExtensions[extIndex]);

        WIN32_FIND_DATAA data;
        HANDLE findHandle = FindFirstFileA(assemblyPath.c_str(), &data);

        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {

                    std::string filename(data.cFileName);
                    size_t extPos = filename.length() - extLength;
                    std::string filenameWithoutExt(filename.substr(0, extPos));

                    // Make sure if we have an assembly with multiple extensions present,
                    // we insert only one version of it.
                    if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
                    {
                        addedAssemblies.insert(filenameWithoutExt);

                        tpaList.append(directory);
                        tpaList.append("\\");
                        tpaList.append(filename);
                        tpaList.append(";");
                    }
                }
            }
            while (0 != FindNextFileA(findHandle, &data));

            FindClose(findHandle);
        }
    }
}

std::string GetExeAbsPath()
{
    char hostPath[MAX_LONGPATH + 1];
    if (::GetModuleFileNameA(NULL, hostPath, MAX_LONGPATH) == 0)
    {
        return false;
    }

    return std::string(hostPath);
}

bool SetWorkDir(const std::string &path)
{
    return SetCurrentDirectoryA(path.c_str());
}

void USleep(uint32_t duration)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10*(int32_t)duration); // Convert to 100 nanosecond interval, negative value indicates relative time

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

void *DLOpen(const std::string &path)
{
    return LoadLibraryA(path.c_str());
}

void *DLSym(void *handle, const std::string &name)
{
    return GetProcAddress((HMODULE)handle, name.c_str());
}

void UnsetCoreCLREnv()
{
    _putenv("CORECLR_ENABLE_PROFILING=");
}

bool IORedirectServerHandles::WaitForConnection(uint16_t port)
{
    WSADATA wsa;
    SOCKET newsockfd;
    int clilen;
    struct sockaddr_in serv_addr, cli_addr;

    if (port == 0)
        return false;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    // Use WSASocket with 0 flags to create a socket without FILE_FLAG_OVERLAPPED.
    // This enables the ReadFile function to block on reading from accepted socket.
    DWORD dwFlags = 0;
    SOCKET sockFd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, dwFlags);
    if (sockFd == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    BOOL enable = 1;
    if (setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enable, sizeof(BOOL)) == SOCKET_ERROR)
    {
        ::closesocket(sockFd);
        WSACleanup();
        return false;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (::bind(sockFd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR)
    {
        ::closesocket(sockFd);
        WSACleanup();
        return false;
    }

    ::listen(sockFd, 5);

    CloseHandle(m_realStdInFd);
    CloseHandle(m_realStdOutFd);
    CloseHandle(m_realStdErrFd);
    m_realStdInFd = INVALID_HANDLE_VALUE;
    m_realStdOutFd = INVALID_HANDLE_VALUE;
    m_realStdErrFd = INVALID_HANDLE_VALUE;

    clilen = sizeof(cli_addr);
    newsockfd = ::accept(sockFd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd == INVALID_SOCKET)
    {
        ::closesocket(sockFd);
        WSACleanup();
        return false;
    }

    m_sockFd = sockFd;
    m_clientFd = newsockfd;
    return true;
}

#define BUFSIZE 4096

static std::function<void()> GetFdReadFunction(HANDLE h, std::function<void(std::string)> cb)
{
    return [h, cb]() {
        char buffer[BUFSIZE];

        while (true)
        {
            DWORD dwRead = 0;
            BOOL bSuccess = ReadFile(h, buffer, BUFSIZE, &dwRead, NULL);

            if (!bSuccess || dwRead == 0)
            {
                break;
            }
            cb(std::string(buffer, dwRead));
        }
    };
}

void IORedirectServerHandles::RedirectOutput(
    std::function<void(std::string)> onStdOut,
    std::function<void(std::string)> onStdErr)
{
    SECURITY_ATTRIBUTES saAttr;

    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE newStdOutRd;
    HANDLE newStdOutWr;

    if (!CreatePipe(&newStdOutRd, &newStdOutWr, &saAttr, 0))
        return;
    if (!SetHandleInformation(newStdOutRd, HANDLE_FLAG_INHERIT, 0))
        return;
    if (!SetStdHandle(STD_OUTPUT_HANDLE, newStdOutWr))
        return;

    HANDLE newStdErrRd;
    HANDLE newStdErrWr;

    if (!CreatePipe(&newStdErrRd, &newStdErrWr, &saAttr, 0))
        return;
    if (!SetHandleInformation(newStdErrRd, HANDLE_FLAG_INHERIT, 0))
        return;
    if (!SetStdHandle(STD_ERROR_HANDLE, newStdErrWr))
        return;

    HANDLE newStdInRd;
    HANDLE newStdInWr;

    if (!CreatePipe(&newStdInRd, &newStdInWr, &saAttr, 0))
        return;
    if (!SetHandleInformation(newStdInWr, HANDLE_FLAG_INHERIT, 0))
        return;
    if (!SetStdHandle(STD_INPUT_HANDLE, newStdInRd))
        return;

    m_appStdIn = newStdInWr;

    std::thread(GetFdReadFunction(newStdOutRd, onStdOut)).detach();
    std::thread(GetFdReadFunction(newStdErrRd, onStdErr)).detach();
}

#else

void AddFilesFromDirectoryToTpaList(const std::string &directory, std::string &tpaList)
{
    const char * const tpaExtensions[] = {
                ".ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
                ".dll",
                ".ni.exe",
                ".exe",
                };

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr)
    {
        return;
    }

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (int extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        int extLength = strlen(ext);

        struct dirent* entry;

        // For all entries in the directory
        while ((entry = readdir(dir)) != nullptr)
        {
            // We are interested in files only
            switch (entry->d_type)
            {
            case DT_REG:
                break;

            // Handle symlinks and file systems that do not support d_type
            case DT_LNK:
            case DT_UNKNOWN:
                {
                    std::string fullFilename;

                    fullFilename.append(directory);
                    fullFilename.append("/");
                    fullFilename.append(entry->d_name);

                    struct stat sb;
                    if (stat(fullFilename.c_str(), &sb) == -1)
                    {
                        continue;
                    }

                    if (!S_ISREG(sb.st_mode))
                    {
                        continue;
                    }
                }
                break;

            default:
                continue;
            }

            std::string filename(entry->d_name);

            // Check if the extension matches the one we are looking for
            int extPos = filename.length() - extLength;
            if ((extPos <= 0) || (filename.compare(extPos, extLength, ext) != 0))
            {
                continue;
            }

            std::string filenameWithoutExt(filename.substr(0, extPos));

            // Make sure if we have an assembly with multiple extensions present,
            // we insert only one version of it.
            if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
            {
                addedAssemblies.insert(filenameWithoutExt);

                tpaList.append(directory);
                tpaList.append("/");
                tpaList.append(filename);
                tpaList.append(":");
            }
        }

        // Rewind the directory stream to be able to iterate over it for the next extension
        rewinddir(dir);
    }

    closedir(dir);
}

std::string GetExeAbsPath()
{
#if defined(__APPLE__)
    // On Mac, we ask the OS for the absolute path to the entrypoint executable
    uint32_t lenActualPath = 0;
    if (_NSGetExecutablePath(nullptr, &lenActualPath) == -1)
    {
        // OSX has placed the actual path length in lenActualPath,
        // so re-attempt the operation
        std::string resizedPath(lenActualPath, '\0');
        char *pResizedPath = const_cast<char *>(resizedPath.data());
        if (_NSGetExecutablePath(pResizedPath, &lenActualPath) == 0)
        {
            return pResizedPath;
        }
    }
    return std::string();
#else
    static const char* self_link = "/proc/self/exe";

    char exe[PATH_MAX];

    ssize_t r = readlink(self_link, exe, PATH_MAX - 1);

    if (r < 0)
    {
        return std::string();
    }

    exe[r] = '\0';

    return exe;
#endif
}

bool SetWorkDir(const std::string &path)
{
    return chdir(path.c_str()) == 0;
}

void USleep(uint32_t duration)
{
    usleep(duration);
}

void *DLOpen(const std::string &path)
{
    return dlopen(path.c_str(), RTLD_GLOBAL | RTLD_NOW);
}

void *DLSym(void *handle, const std::string &name)
{
    return dlsym(handle, name.c_str());
}

void UnsetCoreCLREnv()
{
    unsetenv("CORECLR_ENABLE_PROFILING");
}

bool IORedirectServerHandles::WaitForConnection(uint16_t port)
{
    int newsockfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;

    if (port == 0)
        return false;

    int sockFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0)
        return false;

    int enable = 1;
    if (setsockopt(sockFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        ::close(sockFd);
        return false;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (::bind(sockFd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::close(sockFd);
        return false;
    }

    ::listen(sockFd, 5);

    // On Tizen, launch_app won't terminate until stdin, stdout and stderr are closed.
    // But Visual Studio initiates the connection only after launch_app termination,
    // therefore we need to close the descriptors before the call to accept().
    close(m_realStdInFd);
    close(m_realStdOutFd);
    close(m_realStdErrFd);
    m_realStdInFd = -1;
    m_realStdOutFd = -1;
    m_realStdErrFd = -1;

    clilen = sizeof(cli_addr);
    newsockfd = ::accept(sockFd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0)
    {
        ::close(sockFd);
        return false;
    }

    m_sockFd = sockFd;
    m_clientFd = newsockfd;
    return true;
}

static std::function<void()> GetFdReadFunction(int fd, std::function<void(std::string)> cb)
{
    return [fd, cb]() {
        char buffer[PIPE_BUF];

        while (true)
        {
            //Read up to PIPE_BUF bytes of what's currently at the stdin
            ssize_t read_size = read(fd, buffer, PIPE_BUF);
            if (read_size <= 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            cb(std::string(buffer, read_size));
        }
    };
}

void IORedirectServerHandles::RedirectOutput(
    std::function<void(std::string)> onStdOut,
    std::function<void(std::string)> onStdErr)
{
    // TODO: fcntl(fd, F_SETFD, FD_CLOEXEC);
    m_realStdInFd = dup(STDIN_FILENO);
    m_realStdOutFd = dup(STDOUT_FILENO);
    m_realStdErrFd = dup(STDERR_FILENO);

    int inPipe[2];
    int outPipe[2];
    int errPipe[2];

    if (pipe(inPipe) == -1) return;
    if (pipe(outPipe) == -1) return;
    if (pipe(errPipe) == -1) return;

    if (dup2(inPipe[0], STDIN_FILENO) == -1) return;
    if (dup2(outPipe[1], STDOUT_FILENO) == -1) return;
    if (dup2(errPipe[1], STDERR_FILENO) == -1) return;

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    m_appStdIn = inPipe[1];

    std::thread(GetFdReadFunction(outPipe[0], onStdOut)).detach();
    std::thread(GetFdReadFunction(errPipe[0], onStdErr)).detach();
}

#endif

std::string GetTempFolder()
{
#ifdef WIN32
    CHAR path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH - 1, path);
    return std::string(path, len);
#elif __APPLE__
    char *pPath = getenv("TMPDIR");

    if (pPath != nullptr)
        return std::string(pPath);
    else
        return "";
#else //WIN32
    return "/tmp/";
#endif // WIN32
}

std::string GetBasename(const std::string &path)
{
    std::size_t pos;

#ifdef WIN32
    pos = path.rfind('\\');
#else
    pos = path.rfind('/');
#endif

    if (pos == std::string::npos)
        return std::string(path);

    return std::string(path, pos + 1);
}

bool IsFullPath(const std::string &path)
{
    std::size_t pos;

#ifdef WIN32
    pos = path.rfind('\\');
#else
    pos = path.rfind('/');
#endif

    if (pos == std::string::npos)
        return false;

    return true;
}


IORedirectServer::operator bool() const
{
    return m_handles->IsConnected();
}

IORedirectServer::IORedirectServer(
    uint16_t port,
    std::function<void(std::string)> onStdOut,
    std::function<void(std::string)> onStdErr) :
    m_in(nullptr),
    m_out(nullptr),
    m_handles(new IORedirectServerHandles())
{
    m_handles->RedirectOutput(onStdOut, onStdErr);

    if (m_handles->WaitForConnection(port))
    {
        m_in = new fdbuf(m_handles->GetConnectionHandle());
        m_out = new fdbuf(m_handles->GetConnectionHandle());
    }
    else
    {
        m_in = new fdbuf(m_handles->GetStdIn());
        m_out = new fdbuf(m_handles->GetStdOut());
    }
    m_err = new fdbuf(m_handles->GetStdErr());

    m_prevIn = std::cin.rdbuf();
    m_prevOut = std::cout.rdbuf();
    m_prevErr = std::cerr.rdbuf();

    std::cin.rdbuf(m_in);
    std::cout.rdbuf(m_out);
    std::cerr.rdbuf(m_err);
}

IORedirectServer::~IORedirectServer()
{
    std::cin.rdbuf(m_prevIn);
    std::cout.rdbuf(m_prevOut);
    std::cout.rdbuf(m_prevErr);
    delete m_in;
    delete m_out;
    delete m_err;
    delete m_handles;
}
