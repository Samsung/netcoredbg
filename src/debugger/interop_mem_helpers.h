// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include <cstdint>
#include <sys/types.h>
#include <string>
#include <unordered_map>
#include <functional>

namespace netcoredbg
{
namespace InteropDebugging
{
    typedef std::function<void(const std::string&, std::uintptr_t)> RendListCallback;

    bool ResolveRendezvous(pid_t pid, std::uintptr_t &rendezvousAddr);
    void GetProcessLibs(pid_t pid, std::uintptr_t rendezvousAddr, RendListCallback cb);
    std::uintptr_t GetRendezvousBrkAddr(pid_t pid, std::uintptr_t rendezvousAddr);
    int GetRendezvousBrkState(pid_t pid, std::uintptr_t rendezvousAddr);

    std::uintptr_t GetLibEndAddrAndRealName(pid_t TGID, pid_t pid, std::string &realLibName, std::uintptr_t libAddr);

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
