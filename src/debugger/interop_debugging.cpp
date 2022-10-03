// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_debugging.h"

#include <sys/ptrace.h>
#ifdef DEBUGGER_FOR_TIZEN
// Tizen 5.0/5.5 build fix.
// PTRACE_EVENT_STOP could be absent in old glibc headers, but available in kernel header <linux/ptrace.h>
// instead, since we can't include both of them or use <linux/ptrace.h> only, just define PTRACE_EVENT_STOP.
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif // PTRACE_EVENT_STOP
#endif // DEBUGGER_FOR_TIZEN

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <unordered_map>
#include <thread>
#include <algorithm>
#include <condition_variable>
#include "debugger/waitpid.h"
#include "debugger/sigaction.h"
#include "utils/logger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

namespace
{

    const unsigned int WAITPID_BREAK_AND_DETACH_SIGNAL = SIGUSR2;

    std::mutex g_pidMutex;
    pid_t g_PID = 0;
    pid_t g_needBreakPid = 0;

    std::mutex g_waitpidMutex;
    std::condition_variable g_waitpidCV;
    std::thread g_waitpidWorker;
    int g_waitpidInitError = 0;

    bool joinedWaitpidThread = false;

    enum class thread_stat_e
    {
        stopped,
        running
    };

    struct thread_status_t
    {
        thread_stat_e stat = thread_stat_e::running;
        unsigned int stop_signal = 0;
    };

} // unnamed namespace


// NOTE this method must be called from `ptrace` related thread.
static void WaitAllThreadsStop(std::unordered_map<pid_t, thread_status_t> &TIDs, pid_t tgid)
{
    // At this point all threads must be stopped or interrupted, we need parse all signals now.
    pid_t pid = 0;
    int status = 0;
    while ((pid = GetWaitpid()(-1, &status, __WALL)) > 0)
    {
        if (!WIFSTOPPED(status))
        {
            TIDs.erase(pid);
            continue;
        }

        TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        TIDs[pid].stop_signal = WSTOPSIG(status);
        unsigned event = (unsigned)status >> 16;

        if (TIDs[pid].stop_signal == SIGTRAP)
        {
            if (event != 0)
                TIDs[pid].stop_signal = 0;
            else
            {
                // TODO care about native stepping and breakpoints.
            }

            if (event == PTRACE_EVENT_EXEC && pid != tgid)
            {
                if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1)
                    LOGW("Ptrace detach at exec: %s\n", strerror(errno));
                else
                    TIDs.erase(pid);
            }
        }

        if (std::find_if(TIDs.begin(), TIDs.end(), [](std::pair<pid_t, thread_status_t> entry){return entry.second.stat == thread_stat_e::running;}) == TIDs.end())
            break;
    }
}

// NOTE this method must be called from `ptrace` related thread.
static void StopAndDetach(std::unordered_map<pid_t, thread_status_t> &TIDs, pid_t tgid)
{
    WaitAllThreadsStop(TIDs, tgid);

    for (const auto &tid : TIDs)
    {
        if (ptrace(PTRACE_DETACH, tid.first, nullptr, tid.second.stop_signal) == -1)
            LOGW("Ptrace detach: %s\n", strerror(errno));
    }

    TIDs.clear();
}

// NOTE this method must be called from `ptrace` related thread.
static void Detach(std::unordered_map<pid_t, thread_status_t> &TIDs, pid_t tgid)
{
    for (const auto &tid : TIDs)
    {
        if (tid.second.stat == thread_stat_e::running && ptrace(PTRACE_INTERRUPT, tid.first, nullptr, nullptr) == -1)
            LOGW("Ptrace interrupt: %s\n", strerror(errno));
    }

    StopAndDetach(TIDs, tgid);
}

// Note, InteropDebugging::Shutdown() must be called only in case process stoped or finished.
void Shutdown()
{
    // TODO remove all native breakpoints before `waitpid` break

    // TODO
    // Make sure g_PID is running before send WAITPID_BREAK_AND_DETACH_SIGNAL.
    // Or add logic and send WAITPID_BREAK_AND_DETACH_SIGNAL to thread that is running.

    errno = 0;
    g_pidMutex.lock();
    if (g_PID != 0 && (g_needBreakPid = g_PID) && syscall(SYS_tgkill, g_PID, g_PID, WAITPID_BREAK_AND_DETACH_SIGNAL) == -1)
    {
        g_pidMutex.unlock();
        LOGE("tgkill: %s\n", strerror(errno));
    }
    else
    {
        g_PID = 0; // send only one WAITPID_BREAK_AND_DETACH_SIGNAL
        g_pidMutex.unlock();
        // don't allow join thread more than one time
        if (!joinedWaitpidThread)
        {
            joinedWaitpidThread = true;
            g_waitpidWorker.join();
            GetWaitpid().SetInteropWaitpidMode(false);
        }
    }
}

// NOTE this method must be called from `ptrace` related thread.
static HRESULT SeizeAndInterruptAllThreads(std::unordered_map<pid_t, thread_status_t> &TIDs, const pid_t pid, int &error_n)
{
    char dirname[128];
    if (snprintf(dirname, sizeof dirname, "/proc/%d/task/", pid) >= (int)sizeof(dirname))
    {
        error_n = EFBIG;
        LOGE("Dir name /proc/%d/task/ too long for buffer\n", pid);
        return E_FAIL;
    }

    DIR *dir = opendir(dirname);
    if (!dir)
    {
        error_n = errno;
        LOGE("opendir: %s\n", strerror(errno));
        return E_FAIL;
    }

    const uintptr_t options = PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT | PTRACE_O_TRACECLONE | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEFORK;

    while (true)
    {
        struct dirent *ent;
        int tid;
        char dummy;

        errno = 0;
        ent = readdir(dir);
        if (!ent)
            break;

        if (sscanf(ent->d_name, "%d%c", &tid, &dummy) != 1)
            continue;

        if (tid < 1)
            continue;

        if (ptrace(PTRACE_SEIZE, tid, nullptr, options) == -1)
        {
            error_n = errno;
            LOGE("Ptrace seize: %s\n", strerror(errno));
            closedir(dir);
            return E_FAIL;
        }
        TIDs[tid].stat = thread_stat_e::running; // seize - attach without stop

        if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1)
        {
            LOGE("Ptrace interrupt: %s\n", strerror(errno));
            closedir(dir);
            exit(EXIT_FAILURE); // Fatal error, seized but failed on interrupt.
        }
    }
    if (errno)
    {
        error_n = errno;
        LOGE("readdir: %s\n", strerror(errno));
        closedir(dir);
        return E_FAIL;
    }
    if (closedir(dir))
    {
        LOGW("closedir: %s\n", strerror(errno));
    }

    return S_OK;
}



void WaitpidWorker(pid_t TGID)
{
    std::unordered_map<pid_t, thread_status_t> TIDs;

    int error_n = 0;
    if (FAILED(SeizeAndInterruptAllThreads(TIDs, TGID, error_n)))
    {
        g_waitpidMutex.lock();
        g_waitpidInitError = error_n;
        g_waitpidMutex.unlock();
        // Note, we could attach and interrupt some threads already, must be detached first.
        StopAndDetach(TIDs, TGID);
        g_waitpidCV.notify_one();
        return;
    }

    WaitAllThreadsStop(TIDs, TGID);

    // TODO
    // Care about already loaded native libs.
    // Setup breakpoint and care about native libs load.

    for (auto &tid : TIDs)
    {
        if (ptrace(PTRACE_CONT, tid.first, nullptr, tid.second.stop_signal) == -1)
            LOGW("Ptrace cont error: %s", strerror(errno));
        else
        {
            tid.second.stat = thread_stat_e::running;
            tid.second.stop_signal = 0;
        }
    }

    g_waitpidCV.notify_one();

    pid_t pid = 0;
    int status = 0;
    unsigned event = 0;

    for (;;)
    {
        pid = 0;
        status = 0;
        pid = GetWaitpid()(-1, &status, __WALL);

        if (pid < 0)
            break;

        if (!WIFSTOPPED(status))
        {
            TIDs.erase(pid);

            // Tracee exited or was killed by signal.
            if (pid == TGID)
            {
                GetWaitpid().SetPidExitedStatus(pid, status);
                break;
            }
            else
                continue;
        }

        TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        TIDs[pid].stop_signal = WSTOPSIG(status);
        event = (unsigned)status >> 16;

        // break `waitpid` from outside
        g_pidMutex.lock();
        if (TIDs[pid].stop_signal == WAITPID_BREAK_AND_DETACH_SIGNAL && g_needBreakPid == pid)
        {
            g_pidMutex.unlock();
            break;
        }
        g_pidMutex.unlock();


        // TODO (CoreCLR have sigaction for this signals):
        // SIGSTOP
        // SIGILL
        // SIGFPE
        // SIGSEGV
        // SIGBUS
        // SIGABRT
        // SIGINT - Note, in CLI set to SIG_IGN.
        // SIGQUIT
        // SIGTERM


        if (TIDs[pid].stop_signal == SIGTRAP)
        {
            if (event == PTRACE_EVENT_FORK ||
                event == PTRACE_EVENT_VFORK ||
                event == PTRACE_EVENT_CLONE ||
                event == PTRACE_EVENT_VFORK_DONE ||
                event == PTRACE_EVENT_EXIT ||
                event == PTRACE_EVENT_STOP ||
                event == PTRACE_EVENT_SECCOMP)
            {
                TIDs[pid].stop_signal = 0;
            }
            else if (event == PTRACE_EVENT_EXEC)
            {
                if (pid != TGID)
                {
                    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1)
                        LOGW("Ptrace detach at exec: %s\n", strerror(errno));
                    else
                        TIDs.erase(pid);

                    continue;
                }

                TIDs[pid].stop_signal = 0;
            }
            else if (!event)
            {
                siginfo_t ptrace_info;
                if (ptrace(PTRACE_GETSIGINFO, pid, nullptr, &ptrace_info) == -1)
                {
                    LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
                }
                else
                {
                    switch (ptrace_info.si_code)
                    {
#ifdef SI_KERNEL
                        case SI_KERNEL:
#endif
                        case SI_USER:
                        case TRAP_BRKPT:
                            // TODO native breakpoint
                            // set_pc(get_pc()-1); + fix code
                            // data = DONT_SEND_SIGNAL_NUMBER;
                            break;
                        case TRAP_TRACE:
                            // TODO code single step
                            // data = DONT_SEND_SIGNAL_NUMBER;
                            break;
                    }
                }
            }
        }

        if (ptrace(PTRACE_CONT, pid, nullptr, TIDs[pid].stop_signal) == -1)
            LOGW("Ptrace cont error: %s", strerror(errno));
        else
        {
            TIDs[pid].stat = thread_stat_e::running;
            TIDs[pid].stop_signal = 0;
        }
    }


    g_pidMutex.lock();
    g_PID = 0;
    if (TIDs[pid].stop_signal == WAITPID_BREAK_AND_DETACH_SIGNAL && g_needBreakPid == pid)
    {
        g_pidMutex.unlock();
        TIDs[pid].stop_signal = 0;
        Detach(TIDs, TGID);
    }
    else
        g_pidMutex.unlock();
}

HRESULT Init(pid_t pid, int &error_n)
{
    GetWaitpid().SetInteropWaitpidMode(true);
    GetWaitpid().InitPidStatus((pid_t)pid);

    std::unique_lock<std::mutex> lock(g_waitpidMutex);
    g_waitpidWorker = std::thread(WaitpidWorker, pid);
    g_waitpidCV.wait(lock);
    if (g_waitpidInitError)
    {
        error_n = g_waitpidInitError;
        lock.unlock();

        joinedWaitpidThread = true;
        g_waitpidWorker.join();
        GetWaitpid().SetInteropWaitpidMode(false);
        return E_FAIL;
    }
    lock.unlock();

    joinedWaitpidThread = false;

    g_pidMutex.lock();
    assert(g_PID == 0);
    g_PID = pid;
    g_pidMutex.unlock();

    return S_OK;
}

} // namespace InteropDebugging
} // namespace netcoredbg
