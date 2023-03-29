// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef INTEROP_DEBUGGING

#include <memory>
#include <mutex>
#include <map>
#include <functional>
#include "interfaces/types.h"


namespace elf
{
class elf;
}
namespace dwarf
{
class dwarf;
}

namespace netcoredbg
{
namespace InteropDebugging
{

class InteropLibraries
{
public:

    struct LibraryInfo
    {
        std::string fullName;
        std::uintptr_t libEndAddr; // have same logic as STL `end()` iterator - "first address after"
        // debuginfo related
        std::unique_ptr<elf::elf> ef;
        std::unique_ptr<dwarf::dwarf> dw;
#if DEBUGGER_UNIX_ARM
        // All thumb code related adress blocks in form [`start address`, `end address`),
        // where `start address` is `key` and `end address` is `value` of map.
        bool thumbRegionsValid = false;
        std::map<std::uintptr_t, std::uintptr_t> thumbRegions;
#endif
        // Procedure data in case lib don't have debug info, in this case for stacktrace we need start address (for offset calculation) and name.
        struct proc_data_t
        {
            std::uintptr_t endAddr = 0; // have same logic as STL `end()` iterator - "first address after"
            std::string procName;
            proc_data_t(std::uintptr_t addr, const std::string &name) :
                endAddr(addr),
                procName(name)
            {}
        };
        // Procedure `start address` is `key`.
        bool proceduresDataValid = false;
        std::map<std::uintptr_t, proc_data_t> proceduresData;
        // Is this lib related to CoreCLR (Note, we don't allow debug CoreCLR native code).
        bool isCoreCLR = false;
    };

    void AddLibrary(const std::string &libLoadName, const std::string &fullName, std::uintptr_t startAddr, std::uintptr_t endAddr, SymbolStatus &symbolStatus);
    bool RemoveLibrary(const std::string &fullName, std::uintptr_t &startAddr, std::uintptr_t &endAddr);
    void RemoveAllLibraries();

    std::uintptr_t FindAddrBySourceAndLine(const std::string &fileName, unsigned lineNum, unsigned &resolvedLineNum, std::string &resolvedFullPath, bool &resolvedIsThumbCode);
    std::uintptr_t FindAddrBySourceAndLineForLib(std::uintptr_t startAddr, const std::string &fileName, unsigned lineNum, unsigned &resolvedLineNum, std::string &resolvedFullPath, bool &resolvedIsThumbCode);

    void FindDataForAddr(std::uintptr_t addr, std::string &libName, std::uintptr_t &libStartAddr, std::string &procName,
                         std::uintptr_t &procStartAddr, std::string &fullSourcePath, int &lineNum);
    bool IsThumbCode(std::uintptr_t addr);

private:

    std::mutex m_librariesInfoMutex;
    // We do not provide name to addr mapping, since lib unload is rare and we fine with O(N) in this case.
    // Lib's `start address` is `key`.
    std::map<std::uintptr_t, LibraryInfo> m_librariesInfo;

    void FindLibraryInfoForAddr(std::uintptr_t addr, std::function<void(std::uintptr_t startAddr, LibraryInfo&)> cb);
    bool IsThumbCode(std::uintptr_t libStartAddr, LibraryInfo &info, std::uintptr_t addr);

    // TODO addr search optimization during breakpoint setup by source + line;

};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
