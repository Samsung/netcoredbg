// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_mem_helpers.h"
#include "debugger/interop_ptrace_helpers.h"

#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include "elf++.h"
#include "utils/limits.h"
#include "utils/torelease.h"


namespace netcoredbg
{
namespace InteropDebugging
{

template <class T>
static T ReadFromAddr(pid_t pid, std::uintptr_t &addr)
{
    T t;
    iovec local_iov {&t, sizeof(T)};
    iovec remote_iov {(void*)addr, sizeof(T)};
    if (process_vm_readv(pid, &local_iov, 1, &remote_iov, 1, 0) == -1)
    {
        LOGE("process_vm_readv: %s\n", strerror(errno));
        addr = 0;
        return t;
    }
    addr += sizeof(T);
    return t;
}

static word_t ReadWord(pid_t pid, std::uintptr_t &addr)
{
    return ReadFromAddr<word_t>(pid, addr);
}

static std::string ReadString(pid_t pid, std::uintptr_t startAddr)
{
    std::string result = "";

    bool quit = false;
    while (!quit) // We read null terminated string here by read 4/8 bytes blocks from debuggee process memory.
    {
        word_t word = ReadWord(pid, startAddr);
        unsigned char* wordPtr = reinterpret_cast<unsigned char*>(&word);

        for (size_t i = 0; i < sizeof(word_t); ++i)
        {
            if (!wordPtr[i])
            {
                quit = true;
                break;
            }
            
            result += wordPtr[i];
        }
    };

    return result;
}

static bool GetExecName(pid_t pid, std::string &execName)
{
    char exeFileName[256];
    snprintf(exeFileName, sizeof(exeFileName), "/proc/%d/exe", pid);

    char tmpName[PATH_MAX + 1];
    // https://man7.org/linux/man-pages/man2/readlink.2.html
    // readlink() does not append a terminating null byte to buf. It will (silently) truncate the contents
    // (to a length of bufsiz characters), in case the buffer is too small to hold all of the contents.
    for (int i = 0; i < PATH_MAX + 1; i++)
    {
        tmpName[i] = '\0';
    }

    if (readlink(exeFileName, tmpName, PATH_MAX) == -1)
    {
        LOGE("readlink error for %s file: %s\n", exeFileName, strerror(errno));
        return false;
    }

    execName = tmpName;
    return true;
}

// Note, we need only this process executable file name + start address.
static bool GetProcData(pid_t pid, std::string &execName, std::uintptr_t &startAddr)
{
    execName.clear();
    startAddr = 0;

    if (!GetExecName(pid, execName))
        return false;

    char mapFileName[256];
    snprintf(mapFileName, sizeof(mapFileName), "/proc/%d/task/%d/maps", pid, pid);

    FILE *mapsFile = fopen(mapFileName, "r");
    if (mapsFile == nullptr)
    {
        LOGE("fopen error for %s file: %s\n", mapFileName, strerror(errno));
        return false;
    }

    char *line = nullptr;
    size_t lineLen = 0;
    while (getline(&line, &lineLen, mapsFile) != -1)
    {
        void *startAddress, *endAddress, *offset;
        int devHi, devLo, inode;
        char moduleName[PATH_MAX];

        if (sscanf(line, "%p-%p %*[-rwxsp] %p %x:%x %d %s\n", &startAddress, &endAddress, &offset, &devHi, &devLo, &inode, moduleName) == 7)
        {
            if (inode == 0 || execName != moduleName)
                continue;

            startAddr = (std::uintptr_t)startAddress;
            break;
        }
    }

    free(line); // Note, we did not allocate this, but as per contract of getline we should free it
    fclose(mapsFile);

    if (startAddr == 0)
    {
        LOGE("GetProcData error, can't find in %s start address for %s\n", mapFileName, execName.c_str());
        return false;
    }

    return true;
}

bool ResolveRendezvous(pid_t pid, std::uintptr_t &rendezvousAddr)
{
    std::uintptr_t startAddr;
    std::string elfFileName;
    if (!GetProcData(pid, elfFileName, startAddr))
        return false;

    int fd = open(elfFileName.c_str(), O_RDONLY);
    if (fd == -1)
    {
        LOGE("open error for %s file: %s\n", elfFileName.c_str(), strerror(errno));
        return false;
    }

    std::unique_ptr<elf::elf> elfFile;
    try
    {
        elfFile.reset(new elf::elf(elf::create_mmap_loader(fd)));
    }
    catch(const std::exception &e)
    {
        close(fd);
        LOGE("ResolveRendezvous error at elf parsing: %s\n", e.what());
        return false;
    }
    assert(elfFile != nullptr); // in case of error must throw exception and don't reach this line
    close(fd);

    // find `DYNAMIC` segment:
    std::uintptr_t dynamicAddr = 0;
    for (auto &seg : elfFile->segments())
    {
        if (seg.get_hdr().type == elf::pt::dynamic)
        {
            dynamicAddr = startAddr + seg.get_hdr().vaddr;
            break;
        }
    }
    assert(dynamicAddr != 0); // must have one dynamic segment
    word_t dynamicData = ReadWord(pid, dynamicAddr);

    while (dynamicData != DT_NULL)
    {
        if (dynamicData == DT_DEBUG)
        {
            rendezvousAddr = ReadWord(pid, dynamicAddr);
            return true;
        }
        else
            ReadWord(pid, dynamicAddr); // value or address for current tag

        dynamicData = ReadWord(pid, dynamicAddr);
    }

    return false;
}

void GetProcessLibs(pid_t pid, std::uintptr_t rendezvousAddr, RendListCallback cb)
{
    r_debug rendezvousData = ReadFromAddr<r_debug>(pid, rendezvousAddr);
    link_map *linkMapAddr = rendezvousData.r_map; // linked list of .so entries
    while (linkMapAddr)
    {
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(linkMapAddr);
        link_map map = ReadFromAddr<link_map>(pid, addr);
        std::string name = ReadString(pid, (std::uintptr_t)map.l_name);
        // Note, if name is empty, just ignore it (this is vdso or exec).
        if (name != "")
        {
            cb(name, map.l_addr);
        }
        linkMapAddr = map.l_next;
    }
}

std::uintptr_t GetRendezvousBrkAddr(pid_t pid, std::uintptr_t rendezvousAddr)
{
    r_debug rendezvousData = ReadFromAddr<r_debug>(pid, rendezvousAddr);
    return rendezvousData.r_brk;
}

int GetRendezvousBrkState(pid_t pid, std::uintptr_t rendezvousAddr)
{
    r_debug rendezvousData = ReadFromAddr<r_debug>(pid, rendezvousAddr);
    return rendezvousData.r_state;
}

std::uintptr_t GetLibEndAddrAndRealName(pid_t TGID, pid_t pid, std::string &realLibName, std::uintptr_t libAddr)
{
    char mapFileName[256];
    if (pid)
        snprintf(mapFileName, sizeof(mapFileName), "/proc/%d/task/%d/maps", TGID, pid);
    else
        snprintf(mapFileName, sizeof(mapFileName), "/proc/%d/maps", TGID);

    FILE *mapsFile = fopen(mapFileName, "r");
    if (mapsFile == nullptr)
        return 0;

    std::uintptr_t endAddr = 0;
    assert(realLibName.empty());

    char *line = nullptr;
    size_t lineLen = 0;
    while (getline(&line, &lineLen, mapsFile) != -1)
    {
        void *startAddress, *endAddress, *offset;
        int devHi, devLo, inode;
        char moduleName[PATH_MAX];

        if (sscanf(line, "%p-%p %*[-rwxsp] %p %x:%x %d %s\n", &startAddress, &endAddress, &offset, &devHi, &devLo, &inode, moduleName) == 7)
        {
            if (inode == 0)
                continue;

            if (endAddr != 0 && realLibName != moduleName)
                break;

            if ((std::uintptr_t)startAddress == libAddr)
                realLibName = moduleName;

            if (!realLibName.empty())
                endAddr = (std::uintptr_t)endAddress;
        }
    }

    free(line); // Note, we did not allocate this, but as per contract of getline we should free it
    fclose(mapsFile);

    return endAddr;
}

} // namespace InteropDebugging
} // namespace netcoredbg
