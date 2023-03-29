// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef INTEROP_DEBUGGING

#include <memory>
#include <functional>
#include <unordered_map>
#include "debugger/interop_ptrace_helpers.h"


namespace netcoredbg
{
namespace InteropDebugging
{

class InteropBreakpoints;

class InteropRendezvousBreakpoint
{
public:

    typedef std::function<void(pid_t, const std::string&, const std::string&, std::uintptr_t, std::uintptr_t)> LoadLibCallback;
    typedef std::function<void(const std::string&)> UnloadLibCallback;
    typedef std::function<bool(std::uintptr_t)> IsThumbCodeCallback;

    InteropRendezvousBreakpoint(std::shared_ptr<InteropBreakpoints> &sharedInteropBreakpoints) :
        m_sharedInteropBreakpoints(sharedInteropBreakpoints), m_rendezvousAddr(0), m_rendezvousBrkState(0), m_brkAddr(0)
    {}

    // In case of error - return `false`.
    bool SetupRendezvousBrk(pid_t pid, LoadLibCallback loadLibCB, UnloadLibCallback unloadLibCB, IsThumbCodeCallback isThumbCode, int &err_code);
    bool IsRendezvousBreakpoint(std::uintptr_t brkAddr);
    void ChangeRendezvousState(pid_t TGID, pid_t pid);
    void RemoveAtDetach(pid_t pid);

private:

    std::shared_ptr<InteropBreakpoints> m_sharedInteropBreakpoints;
    std::uintptr_t m_rendezvousAddr;
    int m_rendezvousBrkState;
    std::uintptr_t m_brkAddr;

    LoadLibCallback m_loadLibCB;
    UnloadLibCallback m_unloadLibCB;
    // Mapping for lib's name stored in rendezvous linked list and real lib's full path.
    std::unordered_map<std::string, std::string> m_libsNameToRealNameMap;

};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
