// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"

#include <sys/types.h>
#include <string>
#include <unordered_map>
#include <functional>

namespace netcoredbg
{

#ifdef INTEROP_DEBUGGING
namespace InteropDebugging
{
    typedef std::function<void(const std::string&, std::uintptr_t)> RendListCallback;

    HRESULT ResolveRendezvous(pid_t pid, std::uintptr_t &rendezvousAddr);
    void GetProcessLibs(pid_t pid, std::uintptr_t rendezvousAddr, RendListCallback cb);
    std::uintptr_t GetRendezvousBrkAddr(pid_t pid, std::uintptr_t rendezvousAddr);
    int GetRendezvousBrkState(pid_t pid, std::uintptr_t rendezvousAddr);

} // namespace InteropDebugging
#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
