// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/waitpid.h"

#ifdef FEATURE_PAL
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

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

#ifdef INTEROP_DEBUGGING

void waitpid_t::SetInteropWaitpidMode(bool mode)
{
    std::lock_guard<std::mutex> mutex_guard(pidMutex);
    interopWaitpidMode = mode;
}

bool waitpid_t::IsInteropWaitpidMode()
{
    std::lock_guard<std::mutex> mutex_guard(pidMutex);
    return interopWaitpidMode;
}

void waitpid_t::InitPidStatus(pid_t pid)
{
    std::lock_guard<std::mutex> mutex_guard(pidMutex);
    pidExited = false;
    pidStatus = 0;
    pidPid = pid;
}

void waitpid_t::SetPidExitedStatus(pid_t pid, int status)
{
    std::lock_guard<std::mutex> mutex_guard(pidMutex);
    if (pidPid != pid)
        return;

    pidExited = true;
    pidStatus = status;

    if (WIFEXITED(pidStatus))
    {
        SetExitCode(pid, WEXITSTATUS(pidStatus));
    }
    else if (WIFSIGNALED(pidStatus))
    {
        LOGW("Process terminated without exiting, can't get exit code. Killed by signal %d. Assuming EXIT_FAILURE.", WTERMSIG(pidStatus));
        SetExitCode(pid, EXIT_FAILURE);
    }
    else
    {
        SetExitCode(pid, 0);
    }
}

bool waitpid_t::GetPidExitedStatus(pid_t &pid, int &status)
{
    std::lock_guard<std::mutex> mutex_guard(pidMutex);
    status = pidStatus;
    pid = pidPid;
    return pidExited;
}

#endif // INTEROP_DEBUGGING

waitpid_t waitpid;

} // namespace hook

hook::waitpid_t &GetWaitpid()
{
    return hook::waitpid;
}

// Note, we guaranty `waitpid()` hook works only during debuggee process execution, it aimed to work only for PAL's `waitpid()` calls interception.
extern "C" pid_t waitpid(pid_t pid, int *status, int options)
{
#ifdef INTEROP_DEBUGGING
    if (netcoredbg::hook::waitpid.IsInteropWaitpidMode())
    {
        // Note, we support only `WNOHANG`, dbgshim don't need other options support.
        if (options != WNOHANG)
        {
            errno = EINVAL;
            return -1;
        }

        pid_t pidPid = 0;
        int pidStatus = 0;
        if (!netcoredbg::hook::waitpid.GetPidExitedStatus(pidPid, pidStatus))
        {
            return 0;
        }
        else if (pidPid != pid) // Note, we support only one PID status, dbgshim don't need other PIDs (TIDs) statuses.
        {
            errno = ESRCH;
            return -1;
        }
        else
        {
            if (status)
                *status = pidStatus;

            return pid;
        }
    }
    else if ((options & WNOHANG) != WNOHANG) // Don't allow block waiting in case interop debugging.
    {
        errno = EINVAL;
        return -1;
    }
#endif // INTEROP_DEBUGGING
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
            LOGW("Process terminated without exiting, can't get exit code. Killed by signal %d. Assuming EXIT_FAILURE.", WTERMSIG(*status));
            netcoredbg::hook::waitpid.SetExitCode(pid, EXIT_FAILURE);
        }
    }

    return pidWaitRetval;
}

// Note, liblttng-ust may call `wait()` at CoreCLR global/static initialization at dlopen() (debugger managed part related).
extern "C" pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

} // namespace netcoredbg

#endif // FEATURE_PAL
