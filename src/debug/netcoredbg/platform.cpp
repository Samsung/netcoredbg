// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "platform.h"

#include <cstring>
#include <set>
#include <fstream>

#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <linux/limits.h>
#endif


unsigned long OSPageSize()
{
    static unsigned long pageSize = 0;
    if (pageSize == 0)
        pageSize = sysconf(_SC_PAGESIZE);

    return pageSize;
}

std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

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

// From https://stackoverflow.com/questions/13541313/handle-socket-descriptors-like-file-descriptor-fstream-c-linux
class fdbuf : public std::streambuf
{
private:
    enum { bufsize = 1024 };
    char outbuf_[bufsize];
    char inbuf_[bufsize + 16 - sizeof(int)];
    int  fd_;
public:
    typedef std::streambuf::traits_type traits_type;

    fdbuf(int fd);
    ~fdbuf();
    void open(int fd);
    void close();

protected:
    int overflow(int c);
    int underflow();
    int sync();
};

fdbuf::fdbuf(int fd)
  : fd_(-1) {
    this->open(fd);
}

fdbuf::~fdbuf() {
    this->close();
}

void fdbuf::open(int fd) {
    this->close();
    this->fd_ = fd;
    this->setg(this->inbuf_, this->inbuf_, this->inbuf_);
    this->setp(this->outbuf_, this->outbuf_ + bufsize - 1);
}

void fdbuf::close() {
    if (!(this->fd_ < 0)) {
        this->sync();
        ::close(this->fd_);
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
    if (this->pbase() != this->pptr()) {
        std::streamsize size(this->pptr() - this->pbase());
        std::streamsize done(::write(this->fd_, this->outbuf_, size));
        // The code below assumes that it is success if the stream made
        // some progress. Depending on the needs it may be more
        // reasonable to consider it a success only if it managed to
        // write the entire buffer and, e.g., loop a couple of times
        // to try achieving this success.
        if (0 < done) {
            std::copy(this->pbase() + done, this->pptr(), this->pbase());
            this->setp(this->pbase(), this->epptr());
            this->pbump(size - done);
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
        int done(::read(this->fd_, this->eback() + pback, bufsize));
        this->setg(this->eback(),
                   this->eback() + pback,
                   this->eback() + pback + std::max(0, done));
    }
    return this->gptr() == this->egptr()
        ? traits_type::eof()
        : traits_type::to_int_type(*this->gptr());
}

IORedirectServer::IORedirectServer(uint16_t port) : m_in(nullptr), m_out(nullptr), m_sockfd(-1)
{
    int newsockfd;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    int n;

    m_prevIn = std::cin.rdbuf();
    m_prevOut = std::cout.rdbuf();

    if (port == 0)
        return;

    m_sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_sockfd < 0)
        return;

    int enable = 1;
    if (setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        ::close(m_sockfd);
        m_sockfd = -1;
        return;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (::bind(m_sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ::close(m_sockfd);
        m_sockfd = -1;
        return;
    }

    ::listen(m_sockfd, 5);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    clilen = sizeof(cli_addr);
    newsockfd = ::accept(m_sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0)
    {
        ::close(m_sockfd);
        m_sockfd = -1;
        return;
    }

    m_in = new fdbuf(newsockfd);
    m_out = new fdbuf(newsockfd);

    std::cin.rdbuf(m_in);
    std::cout.rdbuf(m_out);
}

IORedirectServer::~IORedirectServer()
{
    std::cin.rdbuf(m_prevIn);
    std::cout.rdbuf(m_prevOut);
    delete m_in;
    delete m_out;
    ::close(m_sockfd);
}
