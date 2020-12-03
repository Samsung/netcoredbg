// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/waitpid.h"

#ifdef FEATURE_PAL
#include <dlfcn.h>

#include "utils/logger.h"

namespace netcoredbg
{
namespace hook
{

void waitpid_t::init() noexcept
{
    auto ret = dlsym(RTLD_NEXT, "waitpid");
    if (!ret)
    {
        LOGE("Could not find original function waitpid");
        abort();
    }
    original = reinterpret_cast<Signature>(ret);
}

pid_t waitpid_t::operator() (pid_t pid, int *status, int options)
{
    std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
    if (!original)
    {
        init();
    }
    return original(pid, status, options);
}

void waitpid_t::SetupTrackingPID(pid_t PID)
{
    std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
    trackPID = PID;
    exitCode = 0; // same behaviour as CoreCLR have, by default exit code is 0
}

int waitpid_t::GetExitCode()
{
    std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
    return exitCode;
}

void waitpid_t::SetExitCode(pid_t PID, int Code)
{
    std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
    if (trackPID == notConfigured || PID != trackPID)
    {
        return;
    }
    exitCode = Code;
}

waitpid_t waitpid;

} // namespace hook

hook::waitpid_t &GetWaitpid()
{
    return hook::waitpid;
}

// Note, we guaranty waitpid hook works only during debuggee process execution, it aimed to work only for PAL's waitpid calls interception.
extern "C" pid_t waitpid(pid_t pid, int *status, int options) noexcept
{
    pid_t pidWaitRetval = netcoredbg::hook::waitpid(pid, status, options);

    // same logic as PAL have, see PROCGetProcessStatus() and CPalSynchronizationManager::HasProcessExited()
    if (pidWaitRetval == pid)
    {
        if (WIFEXITED(*status))
        {
            netcoredbg::hook::waitpid.SetExitCode(pid, WEXITSTATUS(*status));
        }
        else if (WIFSIGNALED(*status))
        {
            LOGW("Process terminated without exiting; can't get exit code. Killed by signal %d. Assuming EXIT_FAILURE.", WTERMSIG(*status));
            netcoredbg::hook::waitpid.SetExitCode(pid, EXIT_FAILURE);
        }
    }

    return pidWaitRetval;
}

} // namespace netcoredbg

#endif // FEATURE_PAL
