// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoint_interop_rendezvous.h"
#include "debugger/breakpoints_interop.h"
#include "debugger/interop_brk_helpers.h"
#include "debugger/interop_mem_helpers.h"
#include "utils/logger.h"
#include <link.h> // r_debug::r_state enum (RT_CONSISTENT, RT_ADD, RT_DELETE)


namespace netcoredbg
{
namespace InteropDebugging
{

// Must be called only in case all threads stopped during interop initialization.
bool InteropRendezvousBreakpoint::SetupRendezvousBrk(pid_t pid, LoadLibCallback loadLibCB, UnloadLibCallback unloadLibCB, IsThumbCodeCallback isThumbCode, int &err_code)
{
    m_loadLibCB = loadLibCB;
    m_unloadLibCB = unloadLibCB;

    // Libs load/unload related routine initialization.
    // TODO dlmopen() support with multiple load namespaces.
    if (!ResolveRendezvous(pid, m_rendezvousAddr))
    {
        err_code = ENODATA;
        return false;
    }
    GetProcessLibs(pid, m_rendezvousAddr, [this, &pid] (const std::string &libName, std::uintptr_t startAddr)
    {
        std::string realLibName;
        std::uintptr_t endAddr = GetLibEndAddrAndRealName(pid, 0, realLibName, startAddr);
        if (endAddr == 0 || realLibName.empty()) // ignore in case error or linux-vdso.so
            return;
        m_loadLibCB(pid, libName, realLibName, startAddr, endAddr);
        m_libsNameToRealNameMap.emplace(libName, std::move(realLibName));
    });

    // Set break point on function that is called on each library load/unload.
    // For more information, see:
    //     https://sourceware.org/git/?p=glibc.git;a=blob;f=elf/rtld-debugger-interface.txt
    //     https://ypl.coffee/dl-resolve-full-relro/
    m_brkAddr = GetRendezvousBrkAddr(pid, m_rendezvousAddr);
    err_code = m_sharedInteropBreakpoints->Add(pid, m_brkAddr, isThumbCode(m_brkAddr), [](){});
    if (err_code != 0)
        return false;

    m_rendezvousBrkState = GetRendezvousBrkState(pid, m_rendezvousAddr);
    return true;
}

void InteropRendezvousBreakpoint::ChangeRendezvousState(pid_t TGID, pid_t pid)
{
    // The logic is - at first method call type of incoming changed for list provided (add/delete) and at second method call libs list in consistent state.

    int state = GetRendezvousBrkState(TGID, m_rendezvousAddr);

    if (state == r_debug::RT_CONSISTENT)
    {
        if (m_rendezvousBrkState == r_debug::RT_ADD)
        {
            GetProcessLibs(TGID, m_rendezvousAddr, [this, &TGID, &pid] (const std::string &libName, std::uintptr_t startAddr)
            {
                if (m_libsNameToRealNameMap.find(libName) != m_libsNameToRealNameMap.end())
                    return;

                std::string realLibName;
                std::uintptr_t endAddr = GetLibEndAddrAndRealName(TGID, pid, realLibName, startAddr);
                if (endAddr == 0 || realLibName.empty()) // ignore in case error or linux-vdso.so
                    return;
                m_loadLibCB(pid, libName, realLibName, startAddr, endAddr);
                m_libsNameToRealNameMap.emplace(libName, std::move(realLibName));
            });
        }
        else if (m_rendezvousBrkState == r_debug::RT_DELETE)
        {
            auto removeLibs = m_libsNameToRealNameMap;
            GetProcessLibs(TGID, m_rendezvousAddr, [&removeLibs] (const std::string &libName, std::uintptr_t)
            {
                removeLibs.erase(libName);
            });
            for (auto &entry : removeLibs)
            {
                m_unloadLibCB(entry.second);
                m_libsNameToRealNameMap.erase(entry.first);
            }
        }
    }
    m_rendezvousBrkState = state;
}

bool InteropRendezvousBreakpoint::IsRendezvousBreakpoint(std::uintptr_t brkAddr)
{
    if (m_brkAddr != 0 && brkAddr == m_brkAddr && m_sharedInteropBreakpoints->IsBreakpoint(m_brkAddr))
        return true;

    return false;
}

// Must be called only in case all threads stopped and fixed (see InteropDebugger::StopAndDetach()).
void InteropRendezvousBreakpoint::RemoveAtDetach(pid_t pid)
{
    m_sharedInteropBreakpoints->Remove(pid, m_brkAddr, [](){}, [](std::uintptr_t){});

    m_rendezvousAddr = 0;
    m_rendezvousBrkState = 0;
    m_brkAddr = 0;
    m_libsNameToRealNameMap.clear();
}

} // namespace InteropDebugging
} // namespace netcoredbg
