// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef FEATURE_PAL

#include <signal.h>
#include <mutex>

namespace netcoredbg
{
namespace hook
{

// sigaction hook
//
// netcoredbg have managed part and act like corehost for managed part. In the same time,
// CoreCLR have sigaction for SIGCHILD signal, that we need for ptrace/waitpid work.
// By sigaction hook we guaranty, that CoreCLR will not setup sigaction for SIGCHILD and
// don't ruin netcoredbg work with ptrace/waitpid.
// Note, CoreCLR don't setup sigaction for SIGCHILD for common managed code execution
// (netcoredbg usage case), this is part of routine, when CoreCLR have child process.

class sigaction_t
{
private:
    typedef int (*Signature)(int signum, const struct sigaction *act, struct sigaction *oldact);
    Signature original = nullptr;
    std::recursive_mutex interlock;

    sigaction_t(const sigaction_t&) = delete;
    sigaction_t& operator=(const sigaction_t&) = delete;

    void init() noexcept;

public:
    sigaction_t() = default;
    ~sigaction_t() = default;

    int operator() (int signum, const struct sigaction *act, struct sigaction *oldact);

    bool interopDebuggingMode = false;
};

} // namespace hook

void SetSigactionMode(bool interopDebugging);
hook::sigaction_t &GetSigaction();

} // namespace netcoredbg

#endif // FEATURE_PAL
