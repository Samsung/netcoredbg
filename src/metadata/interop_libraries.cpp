// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/interop_libraries.h"

#include <fcntl.h>
#include <unistd.h>
#include "elf++.h"
#include "dwarf++.h"
#include "utils/logger.h"
#if DEBUGGER_UNIX_ARM
#include <elf.h>
#endif


namespace netcoredbg
{
namespace InteropDebugging
{

namespace
{

    constexpr std::uintptr_t NOT_FOUND = 0;

} // unnamed namespace


#if DEBUGGER_UNIX_ARM
void InteropLibraries::CollectThumbCodeRegions(std::uintptr_t startAddr, LibraryInfo &info)
{
    for (auto &sec : info.ef->sections())
    {
        if (sec.get_name() != ".dynsym")
            continue;

        elf::symtab symTab = sec.as_symtab();

        for (auto sym : symTab)
        {
            auto data = sym.get_data();
            if (ELF32_ST_TYPE(data.info) != STT_FUNC || // we need executed code only
                data.size == 0 ||
                (data.value & 1) == 0)                  // skip even offset, odd offset mean Thumb code execution block
                continue;

            std::uintptr_t addrStart = (data.value & ~((std::uintptr_t)1)) + startAddr; // convert to proper (even) address of code block start
            std::uintptr_t addrEnd = addrStart + data.size;

            // merge with next if possible (new block end == some block start)
            auto find = info.thumbRegions.find(addrEnd);
            if (find != info.thumbRegions.end())
            {
                addrEnd = find->second;
                info.thumbRegions.erase(find);
            }

            if (info.thumbRegions.empty() ||
                addrStart > info.thumbRegions.rbegin()->second)
            {
                info.thumbRegions[addrStart] = addrEnd;
                continue;
            }

            // merge with previous if possible (closest lower block end == new block start)
            auto upper_bound = info.thumbRegions.upper_bound(addrStart);
            if (upper_bound != info.thumbRegions.begin())
            {
                auto closest_lower = std::prev(upper_bound);
                if (closest_lower->second == addrStart)
                {
                    closest_lower->second = addrEnd;
                    continue; // in this case we don't need create new entry, since we extend previous
                }
            }

            info.thumbRegions[addrStart] = addrEnd;
        }
    }
}
#endif

bool InteropLibraries::LoadDebuginfoFromFile(std::uintptr_t startAddr, const std::string &fileName, LibraryInfo &info, bool collectElfData)
{
    int fd = open(fileName.c_str(), O_RDONLY);
    if (fd == -1)
    {
        LOGI("Load debuginfo failed at file open %s: %s\n", fileName.c_str(), strerror(errno));
        return false;
    }

    try
    {
        info.ef.reset(new elf::elf(elf::create_mmap_loader(fd)));
    }
    catch(const std::exception& e)
    {
        close(fd);
        LOGI("Load debuginfo failed at elf::elf() for file %s: %s\n", fileName.c_str(), e.what());
        return false;
    }
    close(fd);

    if (collectElfData)
    {
#if DEBUGGER_UNIX_ARM
        CollectThumbCodeRegions(startAddr, info);
#endif
    }

    try
    {
        info.dw.reset(new dwarf::dwarf(dwarf::elf::create_loader(*(info.ef.get()))));
    }
    catch(const std::exception& e)
    {
        info.ef.reset();
        LOGI("Load debuginfo failed at dwarf::dwarf() for file %s: %s\n", fileName.c_str(), e.what());
        return false;
    }

    return true;
};

static bool GetFileNameAndPath(const std::string &path, std::string &fileName, std::string &filePath)
{
    std::size_t i = path.find_last_of("/");
    if (i == std::string::npos)
    {
        LOGE("Only absolute path allowed (this one should be found from /proc/pid/maps), path=%s", path.c_str());
        return false;
    }
    fileName = path.substr(i + 1);
    filePath = path.substr(0, i + 1);
    return true;
}

SymbolStatus InteropLibraries::LoadDebuginfo(std::uintptr_t startAddr, LibraryInfo &info)
{
    // Debuginfo search sequence:
    // 1. Check debuginfo section in target file itself;
    // 2. Check file with same location as target file, but with `.debug` extension;
    // 3. Check file with sub directory `.debug` and with `.debug` extension;
    // 4. Check file with same location as target file inside `/usr/lib/debug/` directory and with `.debug` extension.

    // Note, in case `.so` we also need collect all elf data we could need.
    if (LoadDebuginfoFromFile(startAddr, info.fullName, info, true))
        return SymbolStatus::SymbolsLoaded;

    std::string fileName;
    std::string filePath;
    if (!GetFileNameAndPath(info.fullName, fileName, filePath))
        return SymbolStatus::SymbolsNotFound;

    if (LoadDebuginfoFromFile(startAddr, filePath + fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    if (LoadDebuginfoFromFile(startAddr, filePath + ".debug/"+ fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    if (LoadDebuginfoFromFile(startAddr, "/usr/lib/debug/" + filePath + fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    return SymbolStatus::SymbolsNotFound;
}

void InteropLibraries::AddLibrary(const std::string &fullName, std::uintptr_t startAddr, std::uintptr_t endAddr, SymbolStatus &symbolStatus)
{
    if (endAddr <= startAddr)
    {
        LOGE("End addr must be greater than start addr for %s, library was not added.", fullName.c_str());
        return;
    }

    m_librariesInfoMutex.lock();

    LibraryInfo &info = m_librariesInfo[startAddr];
    info.fullName = fullName;
    info.libEndAddr = endAddr;
    symbolStatus = LoadDebuginfo(startAddr, info);

    m_librariesInfoMutex.unlock();
}

bool InteropLibraries::RemoveLibrary(const std::string &fullName, std::uintptr_t &startAddr, std::uintptr_t &endAddr)
{
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);
    for (auto it = m_librariesInfo.begin(); it != m_librariesInfo.end(); ++it)
    {
        if (it->second.fullName == fullName)
        {
            startAddr = it->first;
            endAddr = it->second.libEndAddr;
            m_librariesInfo.erase(it);
            return true;
        }
    }
    return false;
}

void InteropLibraries::RemoveAllLibraries()
{
    m_librariesInfoMutex.lock();
    m_librariesInfo.clear();
    m_librariesInfoMutex.unlock();
}

static std::uintptr_t FindOffsetBySourceAndLineForDwarf(const std::unique_ptr<dwarf::dwarf> &dw, const std::string &fileName, unsigned lineNum,
                                                        unsigned &resolvedLineNum, std::string &resolvedFullPath)
{
    if (!dw) // check if lib have debuginfo loaded
        return NOT_FOUND;

    std::uintptr_t tmpAddr = 0;
    unsigned tmpLineNum = 0;
    unsigned tmpColumnNum = 0;
    std::string tmpFullPath;

    for (const auto &cu : dw->compilation_units())
    {
        // Fast check for all file names in CU.
        bool nameFound = false;
        cu.get_line_table().iterate_file_names([&fileName, &nameFound](dwarf::line_table::file* sourceFile)
        {
            if (std::equal(fileName.rbegin(), fileName.rend(), sourceFile->path.rbegin())) // "end with"
            {
                nameFound = true;
                return false;
            }
            return true;
        });
        if (!nameFound)
            continue;

        // All lines check.
        for (const auto &line : cu.get_line_table())
        {
            if (line.end_sequence)
                continue;

            if (fileName.size() > line.file->path.size() ||
                !std::equal(fileName.rbegin(), fileName.rend(), line.file->path.rbegin()) || // "end with"
                line.line < lineNum)
                continue;

            if (!tmpAddr)
            {
                tmpAddr = line.address;
                tmpLineNum = line.line;
                tmpColumnNum = line.column;
                tmpFullPath = line.file->path;
            }
            else if (line.line < tmpLineNum || (line.line == tmpLineNum && line.column < tmpColumnNum))
            {
                tmpAddr = line.address;
                tmpLineNum = line.line;
                tmpColumnNum = line.column;
                tmpFullPath = line.file->path;
            }
        }
    }

    if (tmpAddr)
    {
        resolvedLineNum = tmpLineNum;
        resolvedFullPath = tmpFullPath;
        return tmpAddr;
    }

    return NOT_FOUND;
}

std::uintptr_t InteropLibraries::FindAddrBySourceAndLineForLib(std::uintptr_t libStartAddr, const std::string &fileName, unsigned lineNum,
                                                               unsigned &resolvedLineNum, std::string &resolvedFullPath, bool &resolvedIsThumbCode)
{
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);

    auto find = m_librariesInfo.find(libStartAddr);
    if (find == m_librariesInfo.end())
        return NOT_FOUND;

    std::uintptr_t offset = FindOffsetBySourceAndLineForDwarf(find->second.dw, fileName, lineNum, resolvedLineNum, resolvedFullPath);
    if (offset == NOT_FOUND)
        return NOT_FOUND;

    std::uintptr_t addr = libStartAddr + offset; // lib address + offset for line's code
    resolvedIsThumbCode = IsThumbCode(find->second, addr);
    return addr;
}

std::uintptr_t InteropLibraries::FindAddrBySourceAndLine(const std::string &fileName, unsigned lineNum, unsigned &resolvedLineNum,
                                                         std::string &resolvedFullPath, bool &resolvedIsThumbCode)
{
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);

    for (const auto &debugInfo : m_librariesInfo)
    {
        std::uintptr_t offset = FindOffsetBySourceAndLineForDwarf(debugInfo.second.dw, fileName, lineNum, resolvedLineNum, resolvedFullPath);
        if (offset == NOT_FOUND)
            continue;

        std::uintptr_t addr = debugInfo.first + offset; // lib address + offset for line's code
        resolvedIsThumbCode = IsThumbCode(debugInfo.second, addr);
        return addr;
    }

    return NOT_FOUND;
}

bool InteropLibraries::IsThumbCode(std::uintptr_t addr)
{
#if DEBUGGER_UNIX_ARM
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);

    if (m_librariesInfo.empty() ||
        addr >= m_librariesInfo.rbegin()->second.libEndAddr)
        return false;

    auto upper_bound = m_librariesInfo.upper_bound(addr);
    if (upper_bound != m_librariesInfo.begin())
    {
        auto closest_lower = std::prev(upper_bound);
        if (closest_lower->first <= addr && addr < closest_lower->second.libEndAddr)
            return IsThumbCode(closest_lower->second, addr);
    }
#endif
    return false;
}

bool InteropLibraries::IsThumbCode(const LibraryInfo &info, std::uintptr_t addr)
{
#if DEBUGGER_UNIX_ARM
    if (info.thumbRegions.empty() ||
        addr >= info.thumbRegions.rbegin()->second)
        return false;

    auto upper_bound = info.thumbRegions.upper_bound(addr);
    if (upper_bound != info.thumbRegions.begin())
    {
        auto closest_lower = std::prev(upper_bound);
        if (closest_lower->first <= addr && addr < closest_lower->second)
            return true;
    }
#endif
    return false;
}

} // namespace InteropDebugging
} // namespace netcoredbg
