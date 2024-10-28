// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "metadata/interop_libraries.h"

#include <fcntl.h>
#include <unistd.h>
#include "elf++.h"
#include "dwarf++.h"
#include "utils/logger.h"
#include <elf.h>
#include "utils/filesystem.h"
#include <cxxabi.h> // demangle


namespace netcoredbg
{
namespace InteropDebugging
{

namespace
{

    constexpr std::uintptr_t NOT_FOUND = 0;

} // unnamed namespace

static bool OpenElf(const std::string &file, std::unique_ptr<elf::elf> &ef)
{
    int fd = open(file.c_str(), O_RDONLY);
    if (fd == -1)
    {
        char buf[1024];
        LOGI("Load elf failed at file open %s: %s\n", file.c_str(), ErrGetStr(errno, buf, sizeof(buf)));
        return false;
    }

    try
    {
        ef.reset(new elf::elf(elf::create_mmap_loader(fd)));
    }
    catch(const std::exception& e)
    {
        close(fd);
        LOGI("Load elf failed at elf::elf() for file %s: %s\n", file.c_str(), e.what());
        return false;
    }
    close(fd);
    return true;
}

#if DEBUGGER_UNIX_ARM
static bool CollectThumbCodeRegionsBySymtab(std::uintptr_t startAddr, InteropLibraries::LibraryInfo &info, std::unique_ptr<elf::elf> &ef)
{
    std::map<std::uintptr_t, char> blocksByTypes;
    bool symtabDetected = false;
    for (auto &sec : ef->sections())
    {
        if (sec.get_name() != ".symtab")
            continue;

        symtabDetected = true;
        elf::symtab symTab = sec.as_symtab();

        for (auto sym : symTab)
        {
            auto data = sym.get_data();
            if (ELF32_ST_TYPE(data.info) != STT_NOTYPE ||
                data.size != 0)
                continue;

            std::string symName = sym.get_name();
            if (symName.size() < 2 || symName[0] != '$')
                continue;

            std::uintptr_t addrStart = data.value + startAddr; // address is even, no need to convert
            blocksByTypes[addrStart] = symName[1];
        }
    }

    if (!symtabDetected)
        return false;

    // At this point we have blocksByTypes ordered by address and data will be looks like: `d`, `a`, `a`, `a`, `t`, `a`, `t`, `t`, `a`, `t`, `d`, `d`, ...
    std::uintptr_t thumbStart = 0;
    for (auto &entry : blocksByTypes)
    {
        if (entry.second == 't')
        {
            if (!thumbStart)
                thumbStart = entry.first;
        }
        else if (thumbStart)
        {
            info.thumbRegions[thumbStart] = entry.first;
            thumbStart = 0;
        }
    }
    if (thumbStart)
        info.thumbRegions[thumbStart] = info.libEndAddr;

    info.thumbRegionsValid = true;
    return true;
}

static void CollectThumbCodeRegionsByDynsymtab(std::uintptr_t startAddr, InteropLibraries::LibraryInfo &info, std::unique_ptr<elf::elf> &ef)
{
    for (auto &sec : ef->sections())
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

    info.thumbRegionsValid = true;
}

static void CollectThumbCodeRegions(std::uintptr_t startAddr, InteropLibraries::LibraryInfo &info)
{
    std::unique_ptr<elf::elf> ef;
    if (!OpenElf(info.fullName, ef))
        return;

    // Note, in case of thumb code, ".symtab" provide more info than ".dynsym", since in this case we have all thumb code blocks, even for code without symbol name.
    if (CollectThumbCodeRegionsBySymtab(startAddr, info, ef))
        return;

    CollectThumbCodeRegionsByDynsymtab(startAddr, info, ef);
}
#endif

static void CollectProcDataFromElf(std::uintptr_t startAddr, InteropLibraries::LibraryInfo &info)
{
    std::unique_ptr<elf::elf> ef;
    if (!OpenElf(info.fullName, ef))
        return;

    for (auto &sec : ef->sections())
    {
        if (sec.get_name() != ".dynsym" &&
            sec.get_name() != ".symtab")
            continue;

        elf::symtab symTab = sec.as_symtab();

        for (auto sym : symTab)
        {
            auto data = sym.get_data();
            // Both Elf32_Sym and Elf64_Sym use the same one-byte st_info field.
            if (ELF32_ST_TYPE(data.info) != STT_FUNC || // we need executed code only
                data.size == 0)
                continue;

            std::uintptr_t addrStart = data.value + startAddr;

            if (info.proceduresData.find(addrStart) != info.proceduresData.end())
                continue;

#if DEBUGGER_UNIX_ARM
            addrStart = addrStart & ~((std::uintptr_t)1); // convert to proper (even) address of code block start
#endif
            std::uintptr_t addrEnd = addrStart + data.size;

            info.proceduresData.emplace(std::make_pair(addrStart, InteropLibraries::LibraryInfo::proc_data_t(addrEnd, sym.get_name())));
        }
    }

    info.proceduresDataValid = true;
}

static bool LoadDebuginfoFromFile(const std::string &fileName, InteropLibraries::LibraryInfo &info)
{
    if (!OpenElf(fileName, info.ef))
        return false;

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

static SymbolStatus LoadDebuginfo(const std::string &libLoadName, InteropLibraries::LibraryInfo &info)
{
    // Debuginfo search sequence:
    // 1. Check debuginfo section in target file itself;
    // 2. Check file with same location as target file, but with `.debug` extension;
    // 3. Check file with sub directory `.debug` and with `.debug` extension;
    // 4. Check file with same location as target file inside `/usr/lib/debug/` directory and with `.debug` extension.

    // Note, in case `.so` we also need collect all elf data we could need.
    if (LoadDebuginfoFromFile(info.fullName, info))
        return SymbolStatus::SymbolsLoaded;

    std::string fileName;
    std::string filePath;
    if (!GetFileNameAndPath(info.fullName, fileName, filePath))
        return SymbolStatus::SymbolsNotFound;

    if (LoadDebuginfoFromFile(filePath + fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    if (LoadDebuginfoFromFile(filePath + ".debug/"+ fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    if (LoadDebuginfoFromFile("/usr/lib/debug/" + filePath + fileName + ".debug", info))
        return SymbolStatus::SymbolsLoaded;

    // In case lib installed into directory that is symlink to another directory on target system,
    // but `/usr/lib/debug/_lib_path_` with related to this lib debug info is not symlink.
    std::size_t i = libLoadName.find_last_of("/");
    if (i != std::string::npos)
    {
        filePath = libLoadName.substr(0, i + 1);
        if (LoadDebuginfoFromFile("/usr/lib/debug/" + filePath + fileName + ".debug", info))
            return SymbolStatus::SymbolsLoaded;
    }

    return SymbolStatus::SymbolsNotFound;
}

static bool IsCoreCLRLibrary(const std::string &fullName)
{
    // Could be part of SDK, but will be never part of debuggee process:
    // libdbgshim.so      // 2.1 - 6.0
    // libmscordaccore.so // 2.1+
    // libmscordbi.so     // 2.1+
    // libsos.so          // 2.1
    // libsosplugin.so    // 2.1

    static const std::vector<std::string> clrLibs{
        "libclrjit.so",                                      // 2.1+
        "libcoreclr.so",                                     // 2.1+
        "libcoreclrtraceptprovider.so",                      // 2.1+
        "libhostpolicy.so",                                  // 2.1+
        "System.Globalization.Native.so",                    // 2.1 - 3.1
        "System.Security.Cryptography.Native.OpenSsl.so",    // 2.1 - 3.1
        "System.IO.Compression.Native.so",                   // 2.1 - 3.1
        "System.Net.Security.Native.so",                     // 2.1 - 3.1
        "System.Native.so",                                  // 2.1 - 3.1
        "System.Net.Http.Native.so",                         // 2.1 - 3.1
        "libSystem.Native.so",                               // 5.0+
        "libSystem.IO.Compression.Native.so",                // 5.0+
        "libSystem.Net.Security.Native.so",                  // 5.0+
        "libSystem.Security.Cryptography.Native.OpenSsl.so", // 5.0+
        "libSystem.Globalization.Native.so",                 // 6.0+
        "libclrgc.so",                                       // 7.0+
    };

    for (auto &clrLibName : clrLibs)
    {
        if (clrLibName.size() <= fullName.size() &&
            std::equal(clrLibName.rbegin(), clrLibName.rend(), fullName.rbegin())) // "end with"
            return true;
    }
    return false;
}

void InteropLibraries::AddLibrary(const std::string &libLoadName, const std::string &fullName, std::uintptr_t startAddr, std::uintptr_t endAddr, SymbolStatus &symbolStatus)
{
    if (endAddr <= startAddr)
    {
        LOGE("End addr must be greater than start addr for %s, library was not added.", fullName.c_str());
        return;
    }

    m_librariesInfoMutex.lock();

    LibraryInfo &info = m_librariesInfo[startAddr];
    info.fullName = fullName;
    info.fullLoadName = libLoadName;
    info.libEndAddr = endAddr;
    symbolStatus = LoadDebuginfo(libLoadName, info);
    info.isCoreCLR = IsCoreCLRLibrary(fullName);

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
            if (fileName.size() <= sourceFile->path.size() &&
                std::equal(fileName.rbegin(), fileName.rend(), sourceFile->path.rbegin())) // "end with"
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

    if (find->second.isCoreCLR) // NOTE we don't allow setup breakpoint in CoreCLR native code
        return NOT_FOUND;

    std::uintptr_t offset = FindOffsetBySourceAndLineForDwarf(find->second.dw, fileName, lineNum, resolvedLineNum, resolvedFullPath);
    if (offset == NOT_FOUND)
        return NOT_FOUND;

    std::uintptr_t addr = libStartAddr + offset; // lib address + offset for line's code
    resolvedIsThumbCode = IsThumbCode(libStartAddr, find->second, addr);
    return addr;
}

std::uintptr_t InteropLibraries::FindAddrBySourceAndLine(const std::string &fileName, unsigned lineNum, unsigned &resolvedLineNum,
                                                         std::string &resolvedFullPath, bool &resolvedIsThumbCode)
{
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);

    for (auto &debugInfo : m_librariesInfo)
    {
        if (debugInfo.second.isCoreCLR) // NOTE we don't allow setup breakpoint in CoreCLR native code
            continue;

        std::uintptr_t offset = FindOffsetBySourceAndLineForDwarf(debugInfo.second.dw, fileName, lineNum, resolvedLineNum, resolvedFullPath);
        if (offset == NOT_FOUND)
            continue;

        std::uintptr_t addr = debugInfo.first + offset; // lib address + offset for line's code
        resolvedIsThumbCode = IsThumbCode(debugInfo.first, debugInfo.second, addr);
        return addr;
    }

    return NOT_FOUND;
}

void InteropLibraries::FindLibraryInfoForAddr(std::uintptr_t addr, std::function<void(std::uintptr_t startAddr, LibraryInfo&)> cb)
{
    std::lock_guard<std::mutex> lock(m_librariesInfoMutex);

    if (m_librariesInfo.empty() ||
        addr >= m_librariesInfo.rbegin()->second.libEndAddr)
        return;

    auto upper_bound = m_librariesInfo.upper_bound(addr);
    if (upper_bound != m_librariesInfo.begin())
    {
        auto closest_lower = std::prev(upper_bound);
        if (closest_lower->first <= addr && addr < closest_lower->second.libEndAddr)
            cb(closest_lower->first, closest_lower->second);
    }
}

static bool FindDwarfDieByAddr(const dwarf::die &d, dwarf::taddr addr, dwarf::die &node)
{
    // Scan children first to find most specific DIE
    for (const auto &child : d)
    {
        if (FindDwarfDieByAddr(child, addr, node))
                return true;
    }

    switch (d.tag)
    {
    case dwarf::DW_TAG::subprogram:
    case dwarf::DW_TAG::inlined_subroutine:
        try
        {
            if (die_pc_range(d).contains(addr))
            {
                node = d;
                return true;
            }
        }
        catch (std::out_of_range &e) {}
        catch (dwarf::value_type_mismatch &e) {}
        break;
    default:
        break;
    }

    return false;
}

static void ParseDwarfDie(const dwarf::die &node, std::string &methodName, std::string &methodLinkageName)
{
    for (auto &attr : node.attributes())
    {
        switch (attr.first)
        {
        case dwarf::DW_AT::specification:
            ParseDwarfDie(attr.second.as_reference(), methodName, methodLinkageName);
            break;
        case dwarf::DW_AT::abstract_origin:
            ParseDwarfDie(attr.second.as_reference(), methodName, methodLinkageName);
            break;
        case dwarf::DW_AT::name:
            assert(methodName.empty());
            methodName = to_string(attr.second);
            break;
        case dwarf::DW_AT::linkage_name:
            assert(methodLinkageName.empty());
            methodLinkageName = to_string(attr.second);
        default:
            break;
        }
    }
}

static bool DemangleCXXABI(const char *mangledName, std::string &realName)
{
    // CXX ABI demangle only supported now
    // https://gcc.gnu.org/onlinedocs/libstdc++/libstdc++-html-USERS-4.3/a01696.html
    // https://gcc.gnu.org/onlinedocs/libstdc++/manual/ext_demangling.html
    int demangleStatus;
    char *demangledName;
    demangledName = abi::__cxa_demangle(mangledName, 0, 0, &demangleStatus);
    if (demangledName) // could be not CXX ABI mangle name (for example, plane `C` name)
    {
        realName = demangledName;
        free(demangledName);
        return true;
    }
    return false;
}

static void FindDataForAddrInDebugInfo(dwarf::dwarf *dw, std::uintptr_t addr, std::string &procName, std::string &fullSourcePath, int &lineNum)
{
    if (!dw)
        return;

    // TODO use `.debug_aranges`

    for (auto &cu : dw->compilation_units())
    {
        try
        {
            if (!die_pc_range(cu.root()).contains(addr))
                continue;
        }
        catch (std::out_of_range &e) {continue;}
        catch (dwarf::value_type_mismatch &e) {continue;}

        // Map address to source file and line
        auto &lt = cu.get_line_table();
        auto it = lt.find_address(addr);
        if (it == lt.end())
            return;

        fullSourcePath = it->file->path;
        lineNum = it->line;

        // Map address to method name
        // TODO index/helper/something for looking up address
        dwarf::die node;
        if (FindDwarfDieByAddr(cu.root(), addr, node))
        {
            std::string methodName;
            std::string methodLinkageName;
            ParseDwarfDie(node, methodName, methodLinkageName);

            if (!methodLinkageName.empty())
            {
                if (!DemangleCXXABI(methodLinkageName.c_str(), procName))
                    procName = methodLinkageName + "()";
            }
            else if (!methodName.empty())
                procName = methodName + "()";
            else
                procName = "unknown";

            break;
        }
    }
}

void InteropLibraries::FindDataForAddr(std::uintptr_t addr, std::string &libName, std::uintptr_t &libStartAddr, std::string &procName,
                                       std::uintptr_t &procStartAddr, std::string &fullSourcePath, int &lineNum)
{
    FindLibraryInfoForAddr(addr, [&](std::uintptr_t startAddr, LibraryInfo &info)
    {
        libName = GetBasename(info.fullName);
        libStartAddr = startAddr;

        FindDataForAddrInDebugInfo(info.dw.get(), addr - startAddr, procName, fullSourcePath, lineNum);
        if (!procName.empty())
            return;

        if (!info.proceduresDataValid)
            CollectProcDataFromElf(startAddr, info);

        if (info.proceduresData.empty() ||
            addr >= info.proceduresData.rbegin()->second.endAddr)
            return;

        auto upper_bound = info.proceduresData.upper_bound(addr);
        if (upper_bound != info.proceduresData.begin())
        {
            auto closest_lower = std::prev(upper_bound);
            if (closest_lower->first <= addr && addr < closest_lower->second.endAddr)
            {
                procStartAddr = closest_lower->first;
                if (!DemangleCXXABI(closest_lower->second.procName.c_str(), procName))
                    procName = closest_lower->second.procName + "()";
            }
        }
    });
}

bool InteropLibraries::IsUserDebuggingCode(std::uintptr_t addr)
{
    bool isUserCode = false;
    FindLibraryInfoForAddr(addr, [&](std::uintptr_t startAddr, LibraryInfo &info)
    {
        if (!info.isCoreCLR && info.dw != nullptr)
            isUserCode = true;
    });

    return isUserCode;
}

bool InteropLibraries::IsThumbCode(std::uintptr_t addr)
{
#if DEBUGGER_UNIX_ARM
    bool result = false;
    FindLibraryInfoForAddr(addr, [&](std::uintptr_t startAddr, LibraryInfo &info)
    {
        result = IsThumbCode(startAddr, info, addr);
    });
    return result;
#else
    return false;
#endif // DEBUGGER_UNIX_ARM
}

bool InteropLibraries::IsThumbCode(std::uintptr_t libStartAddr, LibraryInfo &info, std::uintptr_t addr)
{
#if DEBUGGER_UNIX_ARM
    if (!info.thumbRegionsValid)
        CollectThumbCodeRegions(libStartAddr, info);

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
#endif // DEBUGGER_UNIX_ARM
    return false;
}

bool InteropLibraries::FindDataForNotClrAddr(std::uintptr_t addr, std::string &libLoadName, std::string &procName)
{
    bool isUserCode = true;
    FindLibraryInfoForAddr(addr, [&](std::uintptr_t startAddr, LibraryInfo &info)
    {
        if (info.isCoreCLR)
        {
            isUserCode = false;
            return;
        }

        libLoadName = GetBasename(info.fullLoadName);
        // Remove version after ".so" (if load name have it)
        static std::string versionDetect(".so.");
        constexpr size_t versionDetectSize = 4;
        if (libLoadName.size() > versionDetectSize)
        {
            size_t i = libLoadName.rfind(versionDetect);
            if (i != std::string::npos)
                libLoadName = libLoadName.substr(0, i + 3);
        }

        if (info.dw != nullptr)
        {
            std::string fullSourcePath;
            int lineNum;
            FindDataForAddrInDebugInfo(info.dw.get(), addr - startAddr, procName, fullSourcePath, lineNum);
            return;
        }

        if (!info.proceduresDataValid)
            CollectProcDataFromElf(startAddr, info);

        if (info.proceduresData.empty() ||
            addr >= info.proceduresData.rbegin()->second.endAddr)
            return;

        auto upper_bound = info.proceduresData.upper_bound(addr);
        if (upper_bound != info.proceduresData.begin())
        {
            auto closest_lower = std::prev(upper_bound);
            if (closest_lower->first <= addr && addr < closest_lower->second.endAddr)
            {
                if (!DemangleCXXABI(closest_lower->second.procName.c_str(), procName))
                    procName = closest_lower->second.procName + "()";
            }
        }
    });

    return isUserCode;
}

} // namespace InteropDebugging
} // namespace netcoredbg
