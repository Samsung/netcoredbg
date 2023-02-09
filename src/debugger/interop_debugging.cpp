// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_debugging.h"
#include "debugger/interop_brk_helpers.h"
#include "debugger/interop_mem_helpers.h"
#include "debugger/interop_ptrace_helpers.h"

#ifdef DEBUGGER_FOR_TIZEN
// Tizen 5.0/5.5 build fix.
// PTRACE_EVENT_STOP could be absent in old glibc headers, but available in kernel header <linux/ptrace.h>
// instead, since we can't include both of them or use <linux/ptrace.h> only, just define PTRACE_EVENT_STOP.
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif // PTRACE_EVENT_STOP
#endif // DEBUGGER_FOR_TIZEN

#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include <dirent.h>
#include <unistd.h> // usleep

#include <vector>
#include <algorithm>
#include "interfaces/iprotocol.h"
#include "debugger/breakpoints.h"
#include "debugger/waitpid.h"
#include "debugger/callbacksqueue.h"
#include "debugger/evalwaiter.h"
#include "metadata/interop_libraries.h"
#include "utils/logger.h"
#include "elf++.h"
#include "dwarf++.h"
#include "utils/filesystem.h"


namespace netcoredbg
{
namespace InteropDebugging
{

InteropDebugger::InteropDebugger(std::shared_ptr<IProtocol> &sharedProtocol,
                                 std::shared_ptr<Breakpoints> &sharedBreakpoints,
                                 std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
    m_sharedProtocol(sharedProtocol),
    m_sharedBreakpoints(sharedBreakpoints),
    m_uniqueInteropLibraries(new InteropLibraries()),
    m_sharedEvalWaiter(sharedEvalWaiter)
{}

// NOTE caller must care about m_waitpidMutex.
void InteropDebugger::WaitAllThreadsStop()
{
    if (std::find_if(m_TIDs.begin(), m_TIDs.end(), [](std::pair<pid_t, thread_status_t> entry){return entry.second.stat == thread_stat_e::running;}) == m_TIDs.end())
        return;

    // At this point all threads must be stopped or interrupted, we need parse all signals now.
    pid_t pid = 0;
    int status = 0;
    // Note, we ignore errors here and don't check is m_TGID exit or not, since in case m_TGID exited `waitpid` return error and break loop.
    while ((pid = GetWaitpid()(-1, &status, __WALL)) > 0)
    {
        if (!WIFSTOPPED(status))
        {
            m_TIDs.erase(pid);

            // Tracee exited or was killed by signal.
            if (pid == m_TGID)
            {
                assert(m_TIDs.empty());
                m_TGID = 0;
                GetWaitpid().SetPidExitedStatus(pid, status);
            }

            continue;
        }

        m_TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        m_TIDs[pid].stop_signal = WSTOPSIG(status);
        m_TIDs[pid].event = (unsigned)status >> 16;
        m_changedThreads.emplace_back(pid);

        if (std::find_if(m_TIDs.begin(), m_TIDs.end(), [](std::pair<pid_t, thread_status_t> entry){return entry.second.stat == thread_stat_e::running;}) == m_TIDs.end())
            break;
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebugger::StopAndDetach(pid_t tgid)
{
    WaitAllThreadsStop();

    // TODO Reset threads status stopped by native steppers
    // TODO Remove all native steppers

    // Reset threads status stopped by native breakpoint
    for (auto &entry : m_TIDs)
    {
        // get registers (we need PC)
        user_regs_struct regs;
        iovec iov;
        iov.iov_base = &regs;
        iov.iov_len = sizeof(user_regs_struct);
        if (async_ptrace(PTRACE_GETREGSET, entry.first, (void*)NT_PRSTATUS, &iov) == -1)
            LOGW("Ptrace getregset error: %s\n", strerror(errno));

        if (m_sharedBreakpoints->InteropStepPrevToBrk(entry.first, GetBrkAddrByPC(regs)))
        {
            // that was native breakpoint event, reset it
            entry.second.stop_signal = 0;
        }
    }

    m_sharedBreakpoints->InteropRemoveAllAtDetach(tgid);
    m_uniqueInteropLibraries->RemoveAllLibraries();

    for (const auto &tid : m_TIDs)
    {
        if (async_ptrace(PTRACE_DETACH, tid.first, nullptr, (void*)((word_t)tid.second.stop_signal)) == -1)
            LOGW("Ptrace detach error: %s\n", strerror(errno));
    }

    m_TIDs.clear();
    m_changedThreads.clear();
    m_eventedThreads.clear();
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebugger::StopAllRunningThreads()
{
    for (const auto &tid : m_TIDs)
    {
        if (tid.second.stat == thread_stat_e::running &&
            async_ptrace(PTRACE_INTERRUPT, tid.first, nullptr, nullptr) == -1)
        {
            LOGW("Ptrace interrupt error: %s\n", strerror(errno));
        }
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebugger::Detach(pid_t tgid)
{
    StopAllRunningThreads();
    StopAndDetach(tgid);
}

// Note, InteropDebugging::Shutdown() must be called only in case process stoped or finished.
void InteropDebugger::Shutdown()
{
    std::unique_lock<std::mutex> lock(m_waitpidMutex);

    if (m_waitpidThreadStatus == WaitpidThreadStatus::WORK ||
        m_waitpidThreadStatus == WaitpidThreadStatus::FINISHED)
    {
        if (m_waitpidThreadStatus == WaitpidThreadStatus::WORK)
        {
            m_waitpidNeedExit = true;
            m_waitpidCV.notify_one(); // notify for exit from infinite loop (thread may stay and unlock mutex on wait() or usleep())
            m_waitpidCV.wait(lock); // wait for exit from infinite loop
        }

        m_waitpidWorker.join();
        Detach(m_TGID);
        m_TGID = 0;
        m_sharedCallbacksQueue = nullptr;
        GetWaitpid().SetInteropWaitpidMode(false);
        m_waitpidThreadStatus = WaitpidThreadStatus::FINISHED_AND_JOINED;
    }

    lock.unlock();

    async_ptrace_shutdown();
}

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

    errno = 0;
    while (true)
    {
        struct dirent *ent;
        int tid;
        char dummy;

        ent = readdir(dir);
        if (!ent)
            break;

        if (sscanf(ent->d_name, "%d%c", &tid, &dummy) != 1)
            continue;

        if (tid < 1)
            continue;

        if (async_ptrace(PTRACE_SEIZE, tid, nullptr, (void*)options) == -1)
        {
            error_n = errno;
            LOGE("Ptrace seize error: %s\n", strerror(errno));
            closedir(dir);
            return E_FAIL;
        }
        TIDs[tid].stat = thread_stat_e::running; // seize - attach without stop

        if (async_ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) == -1)
        {
            LOGE("Ptrace interrupt error: %s\n", strerror(errno));
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

void InteropDebugger::LoadLib(pid_t pid, const std::string &realLibName, std::uintptr_t startAddr, std::uintptr_t endAddr)
{
    // TODO setup related to this lib native breakpoints

    Module module;
    module.id = ""; // TODO add "The `id` field is an opaque identifier of the library"
    module.name = GetBasename(realLibName);
    module.path = realLibName;
    module.baseAddress = startAddr;
    module.size = endAddr - startAddr;
    m_uniqueInteropLibraries->AddLibrary(module.path, startAddr, endAddr, module.symbolStatus);

    if (module.symbolStatus == SymbolStatus::SymbolsLoaded)
    {
        std::vector<BreakpointEvent> events;
        m_sharedBreakpoints->InteropLoadModule(pid, startAddr, m_uniqueInteropLibraries.get(), events);
        for (const BreakpointEvent &event : events)
            m_sharedProtocol->EmitBreakpointEvent(event);
    }

    m_sharedProtocol->EmitModuleEvent(ModuleEvent(ModuleNew, module));
}

void InteropDebugger::UnloadLib(const std::string &realLibName)
{
    Module module;
    module.id = ""; // TODO add "The `id` field is an opaque identifier of the library"
    module.name = GetBasename(realLibName);
    module.path = realLibName;
    m_sharedProtocol->EmitModuleEvent(ModuleEvent(ModuleRemoved, module));
    std::uintptr_t startAddr = 0;
    std::uintptr_t endAddr = 0;
    if (m_uniqueInteropLibraries->RemoveLibrary(realLibName, startAddr, endAddr))
    {
        std::vector<BreakpointEvent> events;
        m_sharedBreakpoints->InteropUnloadModule(startAddr, endAddr, events);
        for (const BreakpointEvent &event : events)
            m_sharedProtocol->EmitBreakpointEvent(event);
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebugger::ParseThreadsChanges()
{
    if (m_changedThreads.empty())
        return;

    for (auto it = m_changedThreads.begin(); it != m_changedThreads.end(); ++it)
    {
        pid_t &pid = *it;

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

        if (m_TIDs[pid].stop_signal == SIGTRAP)
        {
            switch (m_TIDs[pid].event)
            {
            case PTRACE_EVENT_EXEC:
                if (pid != m_TGID)
                {
                    if (async_ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1)
                        LOGW("Ptrace detach at exec error: %s\n", strerror(errno));
                    else
                        m_TIDs.erase(pid);

                    continue;
                }

                m_TIDs[pid].stop_signal = 0;
                break;
            
            case 0: // not ptrace-related event
                {
                    siginfo_t ptrace_info;
                    if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &ptrace_info) == -1)
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
                            {
                                // get registers (we need real breakpoint address for check)
                                user_regs_struct regs;
                                iovec iov;
                                iov.iov_base = &regs;
                                iov.iov_len = sizeof(user_regs_struct);
                                if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
                                    LOGW("Ptrace getregset error: %s\n", strerror(errno));

                                std::uintptr_t brkAddr = GetBrkAddrByPC(regs);

                                if (m_sharedBreakpoints->IsInteropRendezvousBreakpoint(brkAddr))
                                {
                                    m_sharedBreakpoints->InteropChangeRendezvousState(m_TGID, pid);
                                    m_sharedBreakpoints->InteropStepOverBrk(pid, brkAddr);
                                    m_TIDs[pid].stop_signal = 0;
                                }
                                else if (m_sharedBreakpoints->IsInteropBreakpoint(brkAddr))
                                {
                                    // Ignore breakpoints during managed evaluation.
                                    if (m_sharedEvalWaiter->GetEvalRunningThreadID() == (DWORD)pid)
                                    {
                                        StopAllRunningThreads();
                                        WaitAllThreadsStop();
                                        m_sharedBreakpoints->InteropStepOverBrk(pid, brkAddr);
                                        m_TIDs[pid].stop_signal = 0;
                                        break;
                                    }

                                    m_TIDs[pid].stop_signal = 0;
                                    m_TIDs[pid].stat = thread_stat_e::stopped_breakpoint_event_detected;
                                    m_TIDs[pid].stop_event_data.addr = brkAddr;
                                    m_eventedThreads.emplace_back(pid);
                                }
                                break;
                            }
                            case TRAP_TRACE:
                                // TODO check all native steppers
                                // m_TIDs[pid].stop_signal = 0;
                                break;
                        }
                    }
                }
                break;

            //case PTRACE_EVENT_FORK:
            //case PTRACE_EVENT_VFORK:
            //case PTRACE_EVENT_CLONE:
            //case PTRACE_EVENT_VFORK_DONE:
            //case PTRACE_EVENT_EXIT:
            //case PTRACE_EVENT_STOP:
            //case PTRACE_EVENT_SECCOMP;
            default:
                m_TIDs[pid].stop_signal = 0;
                break;
            }
        }
    }

    // NOTE we use second cycle, since during first (parsing) we may need stop all running threads (for example, in case user breakpoint during eval).
    for (const auto &pid : m_changedThreads)
    {
        if (m_TIDs[pid].stat != thread_stat_e::stopped)
            continue;

        if (async_ptrace(PTRACE_CONT, pid, nullptr, (void*)((word_t)m_TIDs[pid].stop_signal)) == -1)
            LOGW("Ptrace cont error: %s", strerror(errno));
        else
        {
            m_TIDs[pid].stat = thread_stat_e::running;
            m_TIDs[pid].stop_signal = 0;
        }
    }

    m_changedThreads.clear();
}

// NOTE mutexes lock sequence must be CallbacksQueue->InteropDebugger.
void InteropDebugger::ParseThreadsEvents()
{
    m_sharedCallbacksQueue->AddInteropCallbackToQueue([&]()
    {
        std::lock_guard<std::mutex> lock(m_waitpidMutex);

        if (m_eventedThreads.empty())
            return;

        for (const auto &pid : m_eventedThreads)
        {
            switch (m_TIDs[pid].stat)
            {
            case thread_stat_e::stopped_breakpoint_event_detected:
                m_sharedCallbacksQueue->EmplaceBackInterop(CallbackQueueCall::InteropBreakpoint, pid, m_TIDs[pid].stop_event_data.addr);
                m_TIDs[pid].stat = thread_stat_e::stopped_breakpoint_event_in_progress;
                break;

            default:
                LOGW("This event type is not stop event: %d", m_TIDs[pid].stat);
                break;
            }
        }

        m_eventedThreads.clear();
    });
}

void InteropDebugger::ContinueAllThreadsWithEvents()
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    bool allThreadsWereStopped = false;

    for (auto &tid : m_TIDs)
    {
        switch (tid.second.stat)
        {
        case thread_stat_e::stopped_breakpoint_event_in_progress:
            BrkStopAllThreads(allThreadsWereStopped);
            m_sharedBreakpoints->InteropStepOverBrk(tid.first, tid.second.stop_event_data.addr);
            break;
        default:
            continue;
        }

        if (async_ptrace(PTRACE_CONT, tid.first, nullptr, nullptr) == -1)
            LOGW("Ptrace cont error: %s", strerror(errno));
        else
        {
            tid.second.stat = thread_stat_e::running;
            tid.second.stop_signal = 0;
        }
    }

    // Continue threads execution with care about stop events (CallbacksQueue).
    if (allThreadsWereStopped)
        ParseThreadsChanges();
}

void InteropDebugger::WaitpidWorker()
{
    std::unique_lock<std::mutex> lock(m_waitpidMutex);
    m_waitpidCV.notify_one(); // notify Init(), that WaitpidWorker() thread init complete
    m_waitpidCV.wait(lock); // wait for "respond" and mutex unlock from Init()

    pid_t pid = 0;
    int status = 0;

    while (!m_TIDs.empty())
    {
        pid = GetWaitpid()(-1, &status, __WALL | WNOHANG);

        if (pid == -1)
        {
            LOGE("Waitpid error: %s\n", strerror(errno));
            break;
        }

        if (pid == 0) // No changes (see `waitpid` man for WNOHANG).
        {
            ParseThreadsChanges();

            lock.unlock();
            // NOTE mutexes lock sequence must be CallbacksQueue->InteropDebugger.
            ParseThreadsEvents();
            usleep(50*1000); // sleep 50 ms before next waitpid call
            lock.lock();

            if (m_waitpidNeedExit)
                break;

            continue;
        }

        if (!WIFSTOPPED(status))
        {
            m_TIDs.erase(pid);

            // Tracee exited or was killed by signal.
            if (pid == m_TGID)
            {
                assert(m_TIDs.empty());
                m_TGID = 0;
                GetWaitpid().SetPidExitedStatus(pid, status);
            }

            continue;
        }

        m_TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        m_TIDs[pid].stop_signal = WSTOPSIG(status);
        m_TIDs[pid].event = (unsigned)status >> 16;
        m_changedThreads.emplace_back(pid);
    }

    m_waitpidCV.notify_one(); // notify Shutdown(), that execution exit from WaitpidWorker()
    m_waitpidThreadStatus = WaitpidThreadStatus::FINISHED;
}

HRESULT InteropDebugger::Init(pid_t pid, std::shared_ptr<CallbacksQueue> &sharedCallbacksQueue, int &error_n)
{
    async_ptrace_init();
    GetWaitpid().SetInteropWaitpidMode(true);
    GetWaitpid().InitPidStatus(pid);

    std::unique_lock<std::mutex> lock(m_waitpidMutex);

    auto ExitWithError = [&]() -> HRESULT
    {
        // Note, we could attach and interrupt some threads already, must be detached first.
        StopAndDetach(pid);
        GetWaitpid().SetInteropWaitpidMode(false);
        m_waitpidThreadStatus = WaitpidThreadStatus::UNKNOWN;
        async_ptrace_shutdown();
        return E_FAIL;
    };

    if (FAILED(SeizeAndInterruptAllThreads(m_TIDs, pid, error_n)))
        return ExitWithError();

    WaitAllThreadsStop();

    auto loadLib = [this] (pid_t stop_pid, const std::string &libRealName, std::uintptr_t startAddr, std::uintptr_t endAddr)
    {
        LoadLib(stop_pid, libRealName, startAddr, endAddr);
    };
    auto unloadLib = [this] (const std::string &libRealName)
    {
        UnloadLib(libRealName);
    };
    auto isThumbCode = [this] (std::uintptr_t addr) -> bool
    {
        return m_uniqueInteropLibraries->IsThumbCode(addr);
    };

    // Note, at rendezvous setup, breakpoints for all previous loaded modules will be resolved in LoadModule() callback.
    if (!m_sharedBreakpoints->InteropSetupRendezvousBrk(pid, loadLib, unloadLib, isThumbCode, error_n))
        return ExitWithError();

    m_waitpidNeedExit = false;
    m_waitpidWorker = std::thread(&InteropDebugger::WaitpidWorker, this);
    m_waitpidCV.wait(lock); // wait for init complete from WaitpidWorker()
    m_waitpidThreadStatus = WaitpidThreadStatus::WORK;
    m_TGID = pid;
    m_sharedCallbacksQueue = sharedCallbacksQueue;
    m_waitpidCV.notify_one(); // notify WaitpidWorker() to start infinite loop

    return S_OK;
}

// In order to add or remove breakpoint we must stop all threads first.
void InteropDebugger::BrkStopAllThreads(bool &allThreadsWereStopped)
{
    if (allThreadsWereStopped)
        return;

    StopAllRunningThreads();
    WaitAllThreadsStop();
    allThreadsWereStopped = true;
}

// In case we need remove breakpoint from address, we must care about all threads first, since some threads could break on this breakpoint already.
// Note, at this point we don't need step over breakpoint, since we don't need "fix, step and restore" logic here.
void InteropDebugger::BrkFixAllThreads(std::uintptr_t checkAddr)
{
    for (auto &entry : m_TIDs)
    {
        // get registers (we need PC)
        user_regs_struct regs;
        iovec iov;
        iov.iov_base = &regs;
        iov.iov_len = sizeof(user_regs_struct);
        if (async_ptrace(PTRACE_GETREGSET, entry.first, (void*)NT_PRSTATUS, &iov) == -1)
            LOGW("Ptrace getregset error: %s\n", strerror(errno));

        std::uintptr_t brkAddrByPC = GetBrkAddrByPC(regs);
        if (checkAddr != brkAddrByPC)
            continue;

        if (m_sharedBreakpoints->InteropStepPrevToBrk(entry.first, brkAddrByPC))
        {
            // that was native breakpoint event, reset it
            entry.second.stop_signal = 0;
            // Note, in this point we could already have stop event added, CallbacksQueue will care about this case.
        }
    }
}

HRESULT InteropDebugger::SetLineBreakpoints(const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    bool allThreadsWereStopped = false;
    auto StopAllThreads = [&]() { BrkStopAllThreads(allThreadsWereStopped); };
    auto FixAllThreads = [&](std::uintptr_t checkAddr) { BrkFixAllThreads(checkAddr); };
    HRESULT Status = m_sharedBreakpoints->InteropSetLineBreakpoints(m_TGID, m_uniqueInteropLibraries.get(), filename, lineBreakpoints, breakpoints, StopAllThreads, FixAllThreads);

    // Continue threads execution with care about stop events (CallbacksQueue).
    if (allThreadsWereStopped)
        ParseThreadsChanges();

    return Status;
}

HRESULT InteropDebugger::AllBreakpointsActivate(bool act)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    bool allThreadsWereStopped = false;
    auto StopAllThreads = [&]() { BrkStopAllThreads(allThreadsWereStopped); };
    auto FixAllThreads = [&](std::uintptr_t checkAddr) { BrkFixAllThreads(checkAddr); };
    HRESULT Status = m_sharedBreakpoints->InteropAllBreakpointsActivate(m_TGID, act, StopAllThreads, FixAllThreads);

    // Continue threads execution with care about stop events (CallbacksQueue).
    if (allThreadsWereStopped)
        ParseThreadsChanges();

    return Status;
}

HRESULT InteropDebugger::BreakpointActivate(uint32_t id, bool act)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    bool allThreadsWereStopped = false;
    auto StopAllThreads = [&]() { BrkStopAllThreads(allThreadsWereStopped); };
    auto FixAllThreads = [&](std::uintptr_t checkAddr) { BrkFixAllThreads(checkAddr); };
    HRESULT Status = m_sharedBreakpoints->InteropBreakpointActivate(m_TGID, id, act, StopAllThreads, FixAllThreads);

    // Continue threads execution with care about stop events (CallbacksQueue).
    if (allThreadsWereStopped)
        ParseThreadsChanges();

    return Status;
}

} // namespace InteropDebugging
} // namespace netcoredbg
