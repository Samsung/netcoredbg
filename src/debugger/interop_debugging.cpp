// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_debugging.h"
#include "debugger/interop_brk_helpers.h"
#include "debugger/interop_mem_helpers.h"
#include "debugger/interop_ptrace_helpers.h"
#include "debugger/interop_arm32_singlestep_helpers.h"

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
#include <sstream>
#include "interfaces/iprotocol.h"
#include "debugger/breakpoints.h"
#include "debugger/waitpid.h"
#include "debugger/callbacksqueue.h"
#include "debugger/evalwaiter.h"
#include "debugger/interop_unwind.h"
#include "metadata/interop_libraries.h"
#include "debugger/frames.h"
#include "utils/logger.h"
#include "elf++.h"
#include "dwarf++.h"
#include "utils/filesystem.h"


namespace netcoredbg
{
namespace InteropDebugging
{

namespace
{
    constexpr pid_t g_waitForAllThreads = -1;
} // unnamed namespace


InteropDebuggerBase::InteropDebuggerBase(IProtocol *pProtocol_,
                                         std::shared_ptr<Breakpoints> &sharedBreakpoints,
                                         std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
    pProtocol(pProtocol_),
    m_sharedBreakpoints(sharedBreakpoints),
    m_uniqueInteropLibraries(new InteropLibraries()),
    m_sharedEvalWaiter(sharedEvalWaiter)
{}

InteropDebuggerSignals::InteropDebuggerSignals(IProtocol *pProtocol_,
                                               std::shared_ptr<Breakpoints> &sharedBreakpoints,
                                               std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
    InteropDebuggerBase(pProtocol_, sharedBreakpoints, sharedEvalWaiter)
{}

InteropDebuggerHelpers::InteropDebuggerHelpers(IProtocol *pProtocol_,
                                               std::shared_ptr<Breakpoints> &sharedBreakpoints,
                                               std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
    InteropDebuggerSignals(pProtocol_, sharedBreakpoints, sharedEvalWaiter)
{}

InteropDebugger::InteropDebugger(IProtocol *pProtocol_,
                                 std::shared_ptr<Breakpoints> &sharedBreakpoints,
                                 std::shared_ptr<EvalWaiter> &sharedEvalWaiter) :
    InteropDebuggerHelpers(pProtocol_, sharedBreakpoints, sharedEvalWaiter)
{}

#if DEBUGGER_UNIX_ARM
// NOTE caller must care about m_waitpidMutex.
static bool DoSoftwareSingleStep(pid_t pid, std::unordered_map<pid_t, thread_status_t> &TIDs, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints)
{
    if (!ARM32_DoSoftwareSingleStep(pid, swSingleStepBreakpoints))
    {
        LOGE("Software singlestep initialization error.\n");
        return false;
    }

    if (async_ptrace(PTRACE_CONT, pid, nullptr, nullptr) == -1)
    {
        LOGW("Ptrace cont error: %s", strerror(errno));
        return false;
    }
    else
    {
        TIDs[pid].stat = thread_stat_e::running;
        TIDs[pid].stop_signal = 0;
    }

    return true;
};

static bool DetectBrkForSoftwareSingleStep(pid_t pid, std::unordered_map<pid_t, thread_status_t> &TIDs, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints)
{
    user_regs_struct regs;
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(user_regs_struct);
    if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
    {
        LOGW("Ptrace getregset error: %s\n", strerror(errno));
        ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
        return false;
    }

    std::uintptr_t brkAddr = GetBrkAddrByPC(regs);

    for (auto &entry : swSingleStepBreakpoints)
    {
        if (entry.bpAddr == brkAddr)
        {
            // Note, we don't call SetPrevBrkPC() + ptrace(PTRACE_SETREGSET) here, since arm32 don't need this for sure.
            TIDs[pid].stop_signal = 0;
            break;
        }
    }

    return ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
};
#endif // DEBUGGER_UNIX_ARM

// NOTE caller must care about m_waitpidMutex.
bool InteropDebuggerBase::SingleStepOnBrk(pid_t pid, std::uintptr_t addr)
{
    // We may have situation, when we check thread status with internal structure, but thread already stopped at breakpoint
    // and we send to this thread PTRACE_INTERRUPT (since internal structure was not updated yet at next waitpid() cycle).
    // In this case, at ptrace(PTRACE_SINGLESTEP) thread will be stopped by PTRACE_INTERRUPT and ptrace(PTRACE_SINGLESTEP)
    // call must be repeated in oreder to finally make single step.

    // Another case is signal that landed on stopped thread. For example SIGKILL from user or SIGILL
    // due to wrong instruction that was covered by breakpoint opcode and now executed on single step.

#if DEBUGGER_UNIX_ARM
    std::vector<sw_singlestep_brk_t> swSingleStepBreakpoints;

    if (!m_HWSingleStepSupported)
    {
        if (!DoSoftwareSingleStep(pid, m_TIDs, swSingleStepBreakpoints))
        {
            ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
            return false;
        }
    }
    else
#endif // DEBUGGER_UNIX_ARM
    {
        if (async_ptrace(PTRACE_SINGLESTEP, pid, nullptr, nullptr) == -1)
        {
#if DEBUGGER_UNIX_ARM
            if (errno == EIO)
            {
                m_HWSingleStepSupported = false;
                if (!DoSoftwareSingleStep(pid, m_TIDs, swSingleStepBreakpoints))
                {
                    ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
                    return false;
                }
            }
            else
#endif // DEBUGGER_UNIX_ARM
            {
                LOGE("Ptrace singlestep error: %s\n", strerror(errno));
                return false;
            }
        }
        else
        {
            m_TIDs[pid].stat = thread_stat_e::running;
            m_TIDs[pid].stop_signal = 0;
        }
    }

    WaitThreadStop(pid);

    // Check that we still have this thread alive.
    if (m_TIDs.find(pid) == m_TIDs.end())
    {
#if DEBUGGER_UNIX_ARM
        if (!m_HWSingleStepSupported)
            ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
#endif // DEBUGGER_UNIX_ARM
        return false;
    }

    if (m_TIDs[pid].stop_signal == SIGTRAP &&
        m_TIDs[pid].event == 0) // not ptrace event (breakpoint, step)
    {
        siginfo_t ptrace_info;
        if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &ptrace_info) == -1)
        {
            LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
#if DEBUGGER_UNIX_ARM
            if (!m_HWSingleStepSupported)
                ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints);
#endif // DEBUGGER_UNIX_ARM
            return false;
        }

        switch (ptrace_info.si_code)
        {
        case SI_KERNEL:
        case TRAP_BRKPT:
#if DEBUGGER_UNIX_ARM
            if (!m_HWSingleStepSupported)
            {
                bool parseSucceeded = DetectBrkForSoftwareSingleStep(pid, m_TIDs, swSingleStepBreakpoints);
                if (!parseSucceeded || m_TIDs[pid].stop_signal == 0)
                    return parseSucceeded;
            }
#endif // DEBUGGER_UNIX_ARM
            // Care about `__builtin_debugtrap()` in user code.
            m_TIDs[pid].stat = thread_stat_e::stopped_signal_event_detected;
            m_TIDs[pid].stop_event_data.signal = "SIGTRAP";
            m_eventedThreads.emplace_back(pid);
            return true;

        case TRAP_TRACE: // "hardware" single step
            m_TIDs[pid].stop_signal = 0;
            return true;
        }
    }

#if DEBUGGER_UNIX_ARM
    if (!m_HWSingleStepSupported && !ARM32_RemoveSoftwareSingleStepBreakpoints(pid, swSingleStepBreakpoints))
        return false;
#endif // DEBUGGER_UNIX_ARM

    if (m_TIDs[pid].stop_signal == SIGILL)
    {
        siginfo_t ptrace_info;
        if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &ptrace_info) == -1)
        {
            LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
            return false;
        }

        if (ptrace_info.si_code == TRAP_TRACE)
        {
            // Care about `__builtin_trap()` in user code.
            m_TIDs[pid].stat = thread_stat_e::stopped_signal_event_detected;
            m_TIDs[pid].stop_event_data.signal = "SIGILL";
            m_eventedThreads.emplace_back(pid);
            return true;
        }
    }

    // Got some signal, that must be handled first.
    // In this case we don't execute sigle step again, since this could be SIGILL for initial wrong opcode, but restore breakoint in memory.
    // We will care about this signal and after that will step over breakpoint if this breakpoint happens again on this thread at next code
    // execution continue (take into account, that breakpoint could be removed before next continue, for example).
    m_TIDs[pid].addrStepOverBreakpointFailed = addr;
    return true;
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebuggerBase::WaitThreadStop(pid_t stoppedPid, std::vector<pid_t> *stoppedTreads)
{
    auto AllRequestedThreadsNotRunning = [&]() -> bool
    {
        if (stoppedTreads != nullptr)
        {
            if (std::find_if(stoppedTreads->begin(), stoppedTreads->end(), [this](pid_t entry)
                                                                            {// NOTE some threads could exit, don't create m_TIDs entry by m_TIDs[] request.
                                                                                auto find = m_TIDs.find(entry);
                                                                                return find == m_TIDs.end() ? false : find->second.stat == thread_stat_e::running;
                                                                            }) == stoppedTreads->end())
                return true;
        }
        else if (stoppedPid == g_waitForAllThreads)
        {
            if (std::find_if(m_TIDs.begin(), m_TIDs.end(), [](std::pair<pid_t, thread_status_t> entry){return entry.second.stat == thread_stat_e::running;}) == m_TIDs.end())
                return true;
        }
        else
        {
            // NOTE thread could exit, don't create m_TIDs entry by m_TIDs[] request.
            auto find = m_TIDs.find(stoppedPid);
            if (find == m_TIDs.end() || find->second.stat != thread_stat_e::running)
                return true;
        }
        return false;
    };
    if (AllRequestedThreadsNotRunning())
        return;

    // At this point all threads must be stopped or interrupted, we need parse all signals now.
    pid_t pid = 0;
    int status = 0;
    // Note, we ignore errors here and don't check is m_TGID exit or not, since in case m_TGID exited `AllRequestedThreadsNotRunning()` break loop.
    while ((pid = GetWaitpid()(-1, &status, __WALL)) > 0)
    {
        if (!WIFSTOPPED(status))
        {
            m_TIDs.erase(pid);
            pProtocol->EmitThreadEvent(ThreadEvent(NativeThreadExited, ThreadId(pid), true));

            // Tracee exited or was killed by signal.
            if (pid == m_TGID)
            {
                assert(m_TIDs.empty());
                m_TGID = 0;
                GetWaitpid().SetPidExitedStatus(pid, status);
                m_NotifyLastThreadExited(status);
            }

            if (AllRequestedThreadsNotRunning())
                break;

            continue;
        }

        unsigned stop_signal = WSTOPSIG(status);

        if (stop_signal == (unsigned)SIGRTMIN)
        {
            // Ignore CoreCLR INJECT_ACTIVATION_SIGNAL here, we can't guarantee it will delivered only once and intime.
            // Note, CoreCLR will be Ok in case INJECT_ACTIVATION_SIGNAL will be never delivered and rely on the GCPOLL mechanism, see
            // https://github.com/dotnet/runtime/blob/8f517afeda93e031b3a797a0eb9e6643adcece2f/src/coreclr/vm/threadsuspend.cpp#L3407-L3425
            siginfo_t siginfo;
            bool sendByItself = false;
            if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &siginfo) == -1)
                LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
            else
                sendByItself = (siginfo.si_pid == m_TGID);

            if (sendByItself)
                stop_signal = 0;
        }

        if (m_TIDs.find(pid) == m_TIDs.end())
            pProtocol->EmitThreadEvent(ThreadEvent(NativeThreadStarted, ThreadId(pid), true));

        m_TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        m_TIDs[pid].stop_signal = stop_signal;
        m_TIDs[pid].event = (unsigned)status >> 16;
        m_changedThreads.emplace_back(pid);

        if (AllRequestedThreadsNotRunning())
            break;
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebuggerHelpers::StopAndDetach(pid_t tgid)
{
    WaitThreadStop(g_waitForAllThreads);

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
        {
            LOGW("Ptrace getregset error: %s\n", strerror(errno));
            continue; // Will hope, this thread didn't stopped at breakpoint.
        }

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
static void StopAllRunningThreads(const std::unordered_map<pid_t, thread_status_t> &TIDs)
{
    for (const auto &tid : TIDs)
    {
        if (tid.second.stat == thread_stat_e::running &&
            async_ptrace(PTRACE_INTERRUPT, tid.first, nullptr, nullptr) == -1)
        {
            LOGW("Ptrace interrupt error: %s\n", strerror(errno));
        }
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebuggerHelpers::Detach(pid_t tgid)
{
    StopAllRunningThreads(m_TIDs);
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
            m_waitpidCV.wait(lock); // wait for exit from infinite loop
        }

        m_waitpidWorker.join();
        Detach(m_TGID);
        m_TGID = 0;
        m_NotifyLastThreadExited = std::function<void(int)>{};
        m_sharedCallbacksQueue = nullptr;
        GetWaitpid().SetInteropWaitpidMode(false);
        m_waitpidThreadStatus = WaitpidThreadStatus::FINISHED_AND_JOINED;
    }

    lock.unlock();

    ShutdownNativeFramesUnwind();
    async_ptrace_shutdown();
}

static HRESULT SeizeAndInterruptAllThreads(std::unordered_map<pid_t, thread_status_t> &TIDs, const pid_t pid, bool attach, int &error_n, IProtocol *pProtocol)
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

        pProtocol->EmitThreadEvent(ThreadEvent(attach ? NativeThreadAttached : NativeThreadStarted, ThreadId(tid), true));
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

void InteropDebuggerHelpers::LoadLib(pid_t pid, const std::string &libLoadName, const std::string &realLibName, std::uintptr_t startAddr, std::uintptr_t endAddr)
{
    Module module;
    module.id = ""; // TODO add "The `id` field is an opaque identifier of the library"
    module.name = GetBasename(realLibName);
    module.path = realLibName;
    module.baseAddress = startAddr;
    module.size = endAddr - startAddr;
    m_uniqueInteropLibraries->AddLibrary(libLoadName, realLibName, startAddr, endAddr, module.symbolStatus);

    if (module.symbolStatus == SymbolStatus::SymbolsLoaded)
    {
        std::vector<BreakpointEvent> events;
        m_sharedBreakpoints->InteropLoadModule(pid, startAddr, m_uniqueInteropLibraries.get(), events);
        for (const BreakpointEvent &event : events)
            pProtocol->EmitBreakpointEvent(event);
    }

    pProtocol->EmitModuleEvent(ModuleEvent(ModuleNew, module));
}

void InteropDebuggerHelpers::UnloadLib(const std::string &realLibName)
{
    Module module;
    module.id = ""; // TODO add "The `id` field is an opaque identifier of the library"
    module.name = GetBasename(realLibName);
    module.path = realLibName;
    pProtocol->EmitModuleEvent(ModuleEvent(ModuleRemoved, module));
    std::uintptr_t startAddr = 0;
    std::uintptr_t endAddr = 0;
    if (m_uniqueInteropLibraries->RemoveLibrary(realLibName, startAddr, endAddr))
    {
        std::vector<BreakpointEvent> events;
        m_sharedBreakpoints->InteropUnloadModule(startAddr, endAddr, events);
        for (const BreakpointEvent &event : events)
            pProtocol->EmitBreakpointEvent(event);
    }
}

static bool AddSignalEventForUserCode(pid_t pid, InteropLibraries *pInteropLibraries, const std::string &signal, thread_status_t &threadStatus)
{
    // get registers (we need real breakpoint address for check)
    user_regs_struct regs;
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(user_regs_struct);
    if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
    {
        LOGW("Ptrace getregset error: %s\n", strerror(errno));
        return false;
    }

    // Should be user code only with debug info and ignore CoreCLR libs.
    std::uintptr_t breakAddr = GetBreakAddrByPC(regs);
    if (!pInteropLibraries->IsUserDebuggingCode(breakAddr))
        return false;

    // We need stop event and only at "continue" sent this signal to CoreCLR (let CoreCLR decide).
    threadStatus.stat = thread_stat_e::stopped_signal_event_detected;
    threadStatus.stop_event_data.addr = breakAddr;
    threadStatus.stop_event_data.signal = signal;
    return true;
}

static bool AddSignalEventForCallerInUserCode(pid_t pid, pid_t TGID, InteropLibraries *pInteropLibraries, const std::string &signal, thread_status_t &threadStatus)
{
    // Check, that debuggee send this to itself by PID.
    siginfo_t siginfo;
    memset(&siginfo, 0, sizeof(siginfo_t));
    if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &siginfo) == -1)
    {
        LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
        return false;
    }
    if (siginfo.si_pid != TGID)
        return false;

    // Should be user code only with debug info and ignore CoreCLR libs.
    // Find second frame - raise()/kill() caller.
    // NOTE in case we can't get second frame data we can't guarantee that stop event will not broke CoreCLR debug API.
    int frameCount = 0;
    bool isUserDebuggingCode = false;
    std::uintptr_t breakAddr = 0;
    ThreadStackUnwind(pid, nullptr, [&](std::uintptr_t addr)
    {
        frameCount++;

        // TODO care about case when one thread could send signal to another inside process:
        // kill(getpid(), SIGTRAP);
        // and that thread's second frame will be in user code it the same time.

        // (?) check that top frame is user code / not CoreCLR (but skip frames with "system" libs like libc, libpthread, ...)

        // (?) check lib + method name for first frame:
        // raise(SIGTRAP)                                   -> libpthread-2.31.so` raise()
        // kill(syscall(SYS_gettid), SIGTRAP)               -> libc-2.31.so` kill()
        // tgkill(getpid(), syscall(SYS_gettid), SIGTRAP)   -> libc-2.31.so` tgkill()
        // syscall(SYS_tkill, syscall(SYS_gettid), SIGTRAP) -> libc-2.31.so` syscall()

        if (frameCount == 1)
            breakAddr = addr;
        else
            isUserDebuggingCode = pInteropLibraries->IsUserDebuggingCode(addr);

        return frameCount < 2;
    });
    if (!isUserDebuggingCode)
        return false;

    // We need stop event and only at "continue" sent this signal to CoreCLR (let CoreCLR decide).
    threadStatus.stat = thread_stat_e::stopped_signal_event_detected;
    threadStatus.stop_event_data.addr = breakAddr;
    threadStatus.stop_event_data.signal = signal;
    return true;
}

void InteropDebuggerSignals::Parse_SIGILL(pid_t pid)
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
        case TRAP_TRACE:
            // Care about `__builtin_trap()` in user code.
            if (AddSignalEventForUserCode(pid, m_uniqueInteropLibraries.get(), "SIGILL", m_TIDs[pid]))
                m_eventedThreads.emplace_back(pid);
            break;
        case SI_USER:
        case SI_TKILL:
            // Care about `raise()` and `kill()` for SIGILL in user code.
            if (AddSignalEventForCallerInUserCode(pid, m_TGID, m_uniqueInteropLibraries.get(), "SIGILL", m_TIDs[pid]))
                m_eventedThreads.emplace_back(pid);
            break;
        }
    }
}

void InteropDebuggerSignals::Parse_SIGTRAP__PTRACE_EVENT_EXEC(pid_t pid)
{
    if (pid == m_TGID)
    {
        m_TIDs[pid].stop_signal = 0;
        return;
    }

    if (async_ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == -1)
        LOGW("Ptrace detach at exec error: %s\n", strerror(errno));
    else
        m_TIDs.erase(pid);
}

void InteropDebuggerSignals::Parse_SIGTRAP__NOT_PTRACE_EVENT(pid_t pid)
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
            case SI_KERNEL:
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

                // Step over breakpoint (that previously was failed, since some signal at single step was received).
                // Note, breakpoint with this address could be already deleted and we stop at another breakpoint, make sure addrStepOverBreakpointFailed is reset.
                if (m_TIDs[pid].addrStepOverBreakpointFailed != 0)
                {
                    // Reset must be before `InteropStepOverBrk()` call, since it could change it again.
                    std::uintptr_t addrStepOverBreakpointFailed = m_TIDs[pid].addrStepOverBreakpointFailed;
                    m_TIDs[pid].addrStepOverBreakpointFailed = 0;
                    if (addrStepOverBreakpointFailed == brkAddr)
                    {
                        StopAllRunningThreads(m_TIDs);
                        WaitThreadStop(g_waitForAllThreads);
                        m_sharedBreakpoints->InteropStepOverBrk(pid, brkAddr, [&](pid_t step_pid, std::uintptr_t step_addr) {return SingleStepOnBrk(step_pid, step_addr);});
                        break;
                    }
                }

                if (m_sharedBreakpoints->IsInteropRendezvousBreakpoint(brkAddr))
                {
                    m_sharedBreakpoints->InteropChangeRendezvousState(m_TGID, pid);
                    m_sharedBreakpoints->InteropStepOverBrk(pid, brkAddr, [&](pid_t step_pid, std::uintptr_t step_addr) {return SingleStepOnBrk(step_pid, step_addr);});
                }
                else if (m_sharedBreakpoints->IsInteropBreakpoint(brkAddr))
                {
                    // Ignore breakpoints during managed evaluation.
                    if (m_sharedEvalWaiter->GetEvalRunningThreadID() == (DWORD)pid)
                    {
                        StopAllRunningThreads(m_TIDs);
                        WaitThreadStop(g_waitForAllThreads);
                        m_sharedBreakpoints->InteropStepOverBrk(pid, brkAddr, [&](pid_t step_pid, std::uintptr_t step_addr) {return SingleStepOnBrk(step_pid, step_addr);});
                        break;
                    }

                    m_TIDs[pid].stop_signal = 0;
                    m_TIDs[pid].stat = thread_stat_e::stopped_breakpoint_event_detected;
                    m_TIDs[pid].stop_event_data.addr = brkAddr;
                    m_eventedThreads.emplace_back(pid);
                }
                else
                {
                    // Care about `__builtin_debugtrap()` in user code.
                    if (AddSignalEventForUserCode(pid, m_uniqueInteropLibraries.get(), "SIGTRAP", m_TIDs[pid]))
                        m_eventedThreads.emplace_back(pid);
                }

                break;
            }
            case SI_USER:
            case SI_TKILL:
                // Care about `raise()` and `kill()` for SIGTRAP in user code.
                if (AddSignalEventForCallerInUserCode(pid, m_TGID, m_uniqueInteropLibraries.get(), "SIGTRAP", m_TIDs[pid]))
                    m_eventedThreads.emplace_back(pid);
                break;
            case TRAP_TRACE:
                // TODO check all native steppers
                // m_TIDs[pid].stop_signal = 0;
                // Reset m_TIDs[pid].addrStepOverBreakpointFailed
                break;
        }
    }
}

// NOTE caller must care about m_waitpidMutex.
void InteropDebuggerBase::ParseThreadsChanges()
{
    if (m_changedThreads.empty())
        return;

    static std::unordered_map<unsigned, std::function<void(pid_t pid)>> signalActions
    {
        { 0 , [&](pid_t pid) {
            // From man 2 kill:
            //    If sig is 0, then no signal is sent, but existence and permission
            //    checks are still performed; this can be used to check for the
            //    existence of a process ID or process group ID that the caller is
            //    permitted to signal.
            // Note, we  also use it as previous signal "reset" (signal, that was parsed by debugger and should not be send to debuggee).
        } },
        { SIGILL , [&](pid_t pid) {
            Parse_SIGILL(pid);
        } },
        { SIGTRAP , [&](pid_t pid) {
            switch (m_TIDs[pid].event)
            {
            case PTRACE_EVENT_EXEC:
                Parse_SIGTRAP__PTRACE_EVENT_EXEC(pid);
                break;
            
            case 0: // not ptrace-related event
                Parse_SIGTRAP__NOT_PTRACE_EVENT(pid);
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
        } }

        // TODO (CoreCLR have sigaction for this signals):
        // SIGSTOP
        // SIGFPE
        // SIGSEGV
        // SIGBUS
        // SIGABRT
        // SIGINT - Note, in CLI set to SIG_IGN.
        // SIGQUIT
        // SIGTERM
    };

    for (auto it = m_changedThreads.begin(); it != m_changedThreads.end(); ++it)
    {
        pid_t &pid = *it;

        if (m_TIDs[pid].stat != thread_stat_e::stopped)
            continue;

        auto find = signalActions.find(m_TIDs[pid].stop_signal);
        if (find != signalActions.end())
        {
            find->second(pid);
        }
    }

    // NOTE we use second cycle, since during first (parsing) we may need stop all running threads (for example, in case user breakpoint during eval).
    for (const auto &pid : m_changedThreads)
    {
        if (m_TIDs[pid].stat != thread_stat_e::stopped &&
            m_TIDs[pid].stat != thread_stat_e::stopped_on_event_need_continue)
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

// Separate thread for callbacks setup in order to make waitpid and CoreCLR debug API work in the same time.
void InteropDebuggerBase::CallbackEventWorker()
{
    std::unique_lock<std::mutex> lock(m_callbackEventMutex);
    m_callbackEventCV.notify_one(); // notify WaitpidWorker(), that thread init complete

    while (true)
    {
        m_callbackEventCV.wait(lock); // wait for request from ParseThreadsEvents() or exit request from WaitpidWorker()

        if (m_callbackEventNeedExit)
            break;

        // m_sharedCallbacksQueue's wrapper that care about m_callbacksMutex lock before code execution in lambda.
        m_sharedCallbacksQueue->AddInteropCallbackToQueue([&]()
        {
            for (const auto &entry : m_callbackEvents)
            {
                switch (entry.stat)
                {
                case thread_stat_e::stopped_breakpoint_event_detected:
                    m_sharedCallbacksQueue->EmplaceBackInterop(CallbackQueueCall::InteropBreakpoint, entry.pid, entry.stop_event_data.addr, "");
                    {
                        // Important! Lock sequence must be (1)m_callbackEventMutex -> (2)m_waitpidMutex only!
                        // Important! Lock sequence must be (1)m_callbacksMutex -> (2)m_waitpidMutex only!
                        std::lock_guard<std::mutex> lock(m_waitpidMutex);
                        auto find = m_TIDs.find(entry.pid);
                        if (find != m_TIDs.end())
                           find->second.stat = thread_stat_e::stopped_breakpoint_event_in_progress;
                    }
                    break;
                case thread_stat_e::stopped_signal_event_detected:
                    m_sharedCallbacksQueue->EmplaceBackInterop(CallbackQueueCall::InteropSignal, entry.pid, entry.stop_event_data.addr, entry.stop_event_data.signal);
                    {
                        // Important! Lock sequence must be (1)m_callbackEventMutex -> (2)m_waitpidMutex only!
                        // Important! Lock sequence must be (1)m_callbacksMutex -> (2)m_waitpidMutex only!
                        std::lock_guard<std::mutex> lock(m_waitpidMutex);
                        auto find = m_TIDs.find(entry.pid);
                        if (find != m_TIDs.end())
                           find->second.stat = thread_stat_e::stopped_signal_event_in_progress;
                    }
                    break;
                default:
                    LOGW("This event type is not stop event: %d", entry.stat);
                    break;
                }
            }

            m_callbackEvents.clear();
        });
    }

    m_callbackEventCV.notify_one(); // notify WaitpidWorker(), that execution exit from CallbackEventWorker()
}

// Caller must care about m_waitpidMutex.
void InteropDebuggerBase::ParseThreadsEvents()
{
    if (m_eventedThreads.empty())
        return;

    // NOTE we can't setup callbacks in waitpid thread, since CoreCLR could use native breakpoints in managed threads, some managed threads
    //      could be stopped at CoreCLR's breakpoint and wait for waitpid, but we wait for managed process `Stop()` in the same time.

    // In case m_callbackEventMutex is locked, return to waitpid loop for next cycle.
    // Important! Lock sequence must be (1)m_callbackEventMutex -> (2)m_waitpidMutex only, but with `try_lock()` we fine here.
    if (!m_callbackEventMutex.try_lock())
        return;

    for (const auto &pid : m_eventedThreads)
    {
        switch (m_TIDs[pid].stat)
        {
        case thread_stat_e::stopped_breakpoint_event_detected:
            m_callbackEvents.emplace_back(pid, m_TIDs[pid].stat, m_TIDs[pid].stop_event_data);
            break;
        case thread_stat_e::stopped_signal_event_detected:
            m_callbackEvents.emplace_back(pid, m_TIDs[pid].stat, m_TIDs[pid].stop_event_data);
            break;
        default:
            LOGW("This event type is not stop event: %d", m_TIDs[pid].stat);
            break;
        }
    }

    m_eventedThreads.clear();

    if (!m_callbackEvents.empty())
        m_callbackEventCV.notify_one();

    m_callbackEventMutex.unlock();
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
            m_sharedBreakpoints->InteropStepOverBrk(tid.first, tid.second.stop_event_data.addr, 
                                                    [&](pid_t step_pid, std::uintptr_t step_addr) {return SingleStepOnBrk(step_pid, step_addr);});
            break;
        case thread_stat_e::stopped_signal_event_in_progress:
            tid.second.stat = thread_stat_e::stopped_on_event_need_continue;
            m_changedThreads.emplace_back(tid.first);
            break;
        case thread_stat_e::stopped_on_event_as_native_thread:
            tid.second.stat = thread_stat_e::stopped;
            tid.second.stop_signal = 0;
            m_changedThreads.emplace_back(tid.first);
            break;
        default:
            continue;
        }
    }

    // Continue native code execution with care about stop events (CallbacksQueue).
    // Ignore allThreadsWereStopped status here, since we could have different events not only breakpoints.
    ParseThreadsChanges();
}

static void GetAllManagedThreads(ICorDebugProcess *pProcess, std::map<pid_t, ToRelease<ICorDebugThread>> &allManagedTreads)
{
    ToRelease<ICorDebugThreadEnum> iCorThreadEnum;
    pProcess->EnumerateThreads(&iCorThreadEnum);
    ULONG fetched = 0;
    ToRelease<ICorDebugThread> iCorThread;
    while (SUCCEEDED(iCorThreadEnum->Next(1, &iCorThread, &fetched)) && fetched == 1)
    {
        DWORD tid = 0;
        if (SUCCEEDED(iCorThread->GetID(&tid)))
            allManagedTreads[tid] = iCorThread.Detach();
        else
            iCorThread.Free();
    }
}

static void StopAllManagedThreads(std::unordered_map<pid_t, thread_status_t> &TIDs, std::map<pid_t, ToRelease<ICorDebugThread>> &allManagedTreads,
                                  std::vector<pid_t> &stoppedManagedTreads)
{
    for (const auto &managedThread : allManagedTreads)
    {
        if (TIDs[managedThread.first].stat != thread_stat_e::running)
            continue;

        if (async_ptrace(PTRACE_INTERRUPT, managedThread.first, nullptr, nullptr) == -1)
            LOGW("Ptrace interrupt error: %s\n", strerror(errno));
        else
            stoppedManagedTreads.emplace_back(managedThread.first);
    }
}

static void AnalyzeAllManagedThreadsTopFrame(std::unordered_map<pid_t, thread_status_t> &TIDs,
                                             std::map<pid_t, ToRelease<ICorDebugThread>> &allManagedTreads)
{
    for (const auto &managedThread : allManagedTreads)
    {
        // Note, we could already have some stop event here, that should be parsed separately.
        if (TIDs[managedThread.first].stat != thread_stat_e::stopped)
            continue;

        HRESULT Status;
        ToRelease<ICorDebugThread3> iCorThread3;
        ToRelease<ICorDebugStackWalk> iCorStackWalk;
        ToRelease<ICorDebugFrame> iCorFrame;
        if (FAILED(managedThread.second->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &iCorThread3)) ||
            FAILED(iCorThread3->CreateStackWalk(&iCorStackWalk)) ||
            FAILED(Status = iCorStackWalk->GetFrame(&iCorFrame)))
            continue;

        if (Status == S_FALSE) // S_FALSE - The current frame is a native stack frame.
        {
            TIDs[managedThread.first].stat = thread_stat_e::stopped_on_event_as_native_thread;
            continue;
        }

        // At this point (Status == S_OK).
        // Accordingly to CoreCLR sources, S_OK could be with nulled iCorFrame, that must be skipped.
        // Related to `FrameType::kExplicitFrame` in runtime (skipped frame function with no-frame transition represents)
        if (iCorFrame == NULL)
            continue;

        // Managed frame.
        ToRelease<ICorDebugFunction> iCorFunction;
        if (SUCCEEDED(iCorFrame->GetFunction(&iCorFunction)))
        {
            // In case of optimized managed code, top frame could be native (optimized code could have inlined pinvoke).
            // Note, breakpoint can't be set in optimized managed code and step can't stop here, since this code is not JMC for sure.
            BOOL bJustMyCode;
            ToRelease<ICorDebugFunction2> iCorFunction2;
            if (SUCCEEDED(iCorFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID *) &iCorFunction2)) &&
                SUCCEEDED(iCorFunction2->GetJMCStatus(&bJustMyCode)) &&
                // Check for optimized code. In case of optimized code, JMC status can't be set to TRUE.
                // https://github.com/dotnet/runtime/blob/main/src/coreclr/debug/ee/debugger.cpp#L11257-L11260
                SUCCEEDED(iCorFunction2->SetJMCStatus(TRUE)))
            {
                // Revert back JMC status if need.
                if (bJustMyCode != TRUE)
                    iCorFunction2->SetJMCStatus(bJustMyCode);

                continue; // not optimized code for sure, don't stop thread.
            }

            // Prevent thread native stop in case thread stopped by managed exception.
            ToRelease<ICorDebugThread> iCorThread;
            ToRelease<ICorDebugValue> iCorExceptionValue;
            if (SUCCEEDED(iCorThread3->QueryInterface(IID_ICorDebugThread, (LPVOID *) &iCorThread)) &&
                SUCCEEDED(iCorThread->GetCurrentException(&iCorExceptionValue)) && iCorExceptionValue != nullptr)
                continue;

            TIDs[managedThread.first].stat = thread_stat_e::stopped_on_event_as_native_thread;
            continue;
        }

        ToRelease<ICorDebugNativeFrame> iCorNativeFrame;
        if (SUCCEEDED(iCorFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &iCorNativeFrame)))
            TIDs[managedThread.first].stat = thread_stat_e::stopped_on_event_as_native_thread;
        //else
        //    Some unknown frame, don't stop it during stop event.
    }
}

HRESULT InteropDebugger::StopAllNativeThreads(ICorDebugProcess *pProcess)
{
    if (!pProcess)
        return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    if (m_TIDs.empty())
        return S_OK;

    std::map<pid_t, ToRelease<ICorDebugThread>> allManagedTreads;
    GetAllManagedThreads(pProcess, allManagedTreads);

    std::vector<pid_t> stoppedManagedTreads;
    stoppedManagedTreads.reserve(allManagedTreads.size());
    StopAllManagedThreads(m_TIDs, allManagedTreads, stoppedManagedTreads);
    WaitThreadStop(g_waitForAllThreads, &stoppedManagedTreads);

    AnalyzeAllManagedThreadsTopFrame(m_TIDs, allManagedTreads);
    // At this point we have all managed threads stopped by `ptrace`:
    //    thread_stat_e::stopped                           - execution will continue in ParseThreadsChanges() call in this method
    //    thread_stat_e::stopped_on_event_as_native_thread - execution will continue only after stop event ends

    // Stop all native treads and analyze them (we need stop all not CoreCLR related treads now).

    StopAllRunningThreads(m_TIDs);
    WaitThreadStop(g_waitForAllThreads);

    static std::unordered_set<std::string> unwindStopFrames{
        "libstdc++.so`execute_native_thread_routine()",
        "libpthread.so`start_thread()",
        "libc.so`__libc_start_main()",
        "libc.so`clone()"
    };

    for (auto &nativeThread : m_TIDs)
    {
        // Skip managed threads, we analyzed all of them early.
        if (allManagedTreads.find(nativeThread.first) != allManagedTreads.end())
            continue;

        // Note, we could already have some stop event here, that should be parsed separately.
        if (m_TIDs[nativeThread.first].stat != thread_stat_e::stopped)
            continue;

        bool skipThread = false;
        bool reachedStopFrames = false;
        ThreadStackUnwind(nativeThread.first, nullptr, [&](std::uintptr_t addr)
        {
#if defined(DEBUGGER_UNIX_ARM)
            addr = addr & ~((std::uintptr_t)1); // convert to proper (even) address (debug info use only even addresses)
#endif

            // Note, in this case we don't need info for address that is part of previous (already executed) code (we need only procedure name).
            // This mean, no need address correction here for all frames.

            std::string libLoadName;
            std::string procName;
            if (!m_uniqueInteropLibraries->FindDataForNotClrAddr(addr, libLoadName, procName))
            {
                skipThread = true;
                return false;
            }

            // Case when lib name and/or procedure name can't be gathered, considered to be "user" code (frame should be skipped).
            if (libLoadName.empty() || procName.empty())
                return true;

            // Note, in order to unwind successfully (and detect all user's code related threads), debug info for all user's code should be installed, plus,
            // some arches could miss dynsyms for `start_thread()` or `clone()`, that mean debug info for libc/glibc and libstdc++ should be installed.
            std::string unwindFrame = libLoadName + "`" + procName;
            reachedStopFrames = unwindStopFrames.find(unwindFrame) != unwindStopFrames.end();
            return !reachedStopFrames;
        });

        if (!skipThread && reachedStopFrames)
            m_TIDs[nativeThread.first].stat = thread_stat_e::stopped_on_event_as_native_thread;
    }

    ParseThreadsChanges();
    return S_OK;
}

void InteropDebuggerBase::InitWaitpidWorkerThread()
{
    m_waitpidWorker = std::thread(&InteropDebuggerBase::WaitpidWorker, this);
}

void InteropDebuggerBase::WaitpidWorker()
{
    std::unique_lock<std::mutex> lockEvent(m_callbackEventMutex);
    m_callbackEventNeedExit = false;
    m_callbackEventWorker = std::thread(&InteropDebuggerBase::CallbackEventWorker, this);
    m_callbackEventCV.wait(lockEvent); // wait for init complete from CallbackEventWorker()
    lockEvent.unlock();

    std::unique_lock<std::mutex> lockWaitpid(m_waitpidMutex);
    m_waitpidCV.notify_one(); // notify Init(), that WaitpidWorker() thread init complete
    m_waitpidCV.wait(lockWaitpid); // wait for "respond" and mutex unlock from Init()

    pid_t pid = 0;
    int status = 0;
    std::unordered_map<pid_t, int> injectTIDs; // CoreCLR's INJECT_ACTIVATION_SIGNAL related.
    static const int injectSignalResetCountdown = 5; // 5 * 10 ms

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
            // INJECT_ACTIVATION_SIGNAL could be delivered with some delay and we could have "no signals" return from `waitpid`.
            // In the same time, injectTIDs should be reseted, since after some time next signal also could be INJECT_ACTIVATION_SIGNAL.
            // Note, CoreCLR will be Ok in case INJECT_ACTIVATION_SIGNAL will be never delivered and rely on the GCPOLL mechanism, see
            // https://github.com/dotnet/runtime/blob/8f517afeda93e031b3a797a0eb9e6643adcece2f/src/coreclr/vm/threadsuspend.cpp#L3407-L3425
            for (auto it = injectTIDs.begin(); it != injectTIDs.end();)
            {
                if (it->second == 0)
                    it = injectTIDs.erase(it);
                else
                {
                    it->second--;
                    ++it;
                }
            }

            ParseThreadsChanges();
            ParseThreadsEvents();

            lockWaitpid.unlock();
            usleep(10*1000); // sleep 10 ms before next waitpid call
            lockWaitpid.lock();

            if (m_waitpidNeedExit)
                break;

            continue;
        }

        if (!WIFSTOPPED(status))
        {
            m_TIDs.erase(pid);
            pProtocol->EmitThreadEvent(ThreadEvent(NativeThreadExited, ThreadId(pid), true));

            // Tracee exited or was killed by signal.
            if (pid == m_TGID)
            {
                assert(m_TIDs.empty());
                m_TGID = 0;
                GetWaitpid().SetPidExitedStatus(pid, status);
                m_NotifyLastThreadExited(status);
            }

            continue;
        }

        unsigned stop_signal = WSTOPSIG(status);

        if (stop_signal == (unsigned)SIGRTMIN)
        {
            // CoreCLR could send a lot of INJECT_ACTIVATION_SIGNALs for thread between our `waitpid` calls,
            // in order to start code execution on thread in time. Make sure only one was really send and ignore others.
            // Note, CoreCLR don't expect that bunch of signals will return, it need start related code only once.

            // Note, CoreCLR at INJECT_ACTIVATION_SIGNAL will (from CoreCLR cources): "Only accept activations from the current process".
            siginfo_t siginfo;
            bool sendByItself = false;
            if (async_ptrace(PTRACE_GETSIGINFO, pid, nullptr, &siginfo) == -1)
                LOGW("Ptrace getsiginfo error: %s\n", strerror(errno));
            else
                sendByItself = (siginfo.si_pid == m_TGID);

            if (sendByItself)
            {
                auto find = injectTIDs.find(pid);
                if (find != injectTIDs.end())
                {
                    stop_signal = 0;
                    find->second = injectSignalResetCountdown;
                }
                else
                    injectTIDs.emplace(std::make_pair(pid, injectSignalResetCountdown));

                if (async_ptrace(PTRACE_CONT, pid, nullptr, (void*)((word_t)stop_signal)) == -1)
                    LOGW("Ptrace cont error: %s", strerror(errno));
                // No need change `m_TIDs[pid].stat` and `m_TIDs[pid].stop_signal` here.
                continue;
            }
        }

        if (m_TIDs.find(pid) == m_TIDs.end())
            pProtocol->EmitThreadEvent(ThreadEvent(NativeThreadStarted, ThreadId(pid), true));

        m_TIDs[pid].stat = thread_stat_e::stopped; // if we here, this mean we get some stop signal for this thread
        m_TIDs[pid].stop_signal = stop_signal;
        m_TIDs[pid].event = (unsigned)status >> 16;
        m_changedThreads.emplace_back(pid);
    }

    m_waitpidThreadStatus = WaitpidThreadStatus::FINISHED;
    lockWaitpid.unlock(); // Important! Lock sequence must be (1)m_callbackEventMutex -> (2)m_waitpidMutex only!

    lockEvent.lock();
    m_callbackEventNeedExit = true;
    m_callbackEventCV.notify_one(); // notify CallbackEventWorker() for exit from infinite loop
    m_callbackEventCV.wait(lockEvent); // wait for exit from infinite loop
    m_callbackEventWorker.join();

    m_waitpidCV.notify_one(); // notify Shutdown(), that execution exit from WaitpidWorker()
}

HRESULT InteropDebugger::Init(pid_t pid, std::shared_ptr<CallbacksQueue> &sharedCallbacksQueue, bool attach, std::function<void(int)> NotifyLastThreadExited, int &error_n)
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

    if (FAILED(SeizeAndInterruptAllThreads(m_TIDs, pid, attach, error_n, pProtocol)))
        return ExitWithError();

    WaitThreadStop(g_waitForAllThreads);

    auto loadLib = [this] (pid_t stop_pid, const std::string &libLoadName, const std::string &libRealName, std::uintptr_t startAddr, std::uintptr_t endAddr)
    {
        LoadLib(stop_pid, libLoadName, libRealName, startAddr, endAddr);
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

    // At this point all threads are stopped, continue execution for all not event-related stopped threads.
    ParseThreadsChanges();

    m_waitpidNeedExit = false;
    InitWaitpidWorkerThread();
    m_waitpidCV.wait(lock); // wait for init complete from WaitpidWorker()
    m_waitpidThreadStatus = WaitpidThreadStatus::WORK;
    m_TGID = pid;
    m_sharedCallbacksQueue = sharedCallbacksQueue;
    m_waitpidCV.notify_one(); // notify WaitpidWorker() to start infinite loop

    m_NotifyLastThreadExited = NotifyLastThreadExited;
    InitNativeFramesUnwind(this);
    return S_OK;
}

// In order to add or remove breakpoint we must stop all threads first.
void InteropDebuggerHelpers::BrkStopAllThreads(bool &allThreadsWereStopped)
{
    if (allThreadsWereStopped)
        return;

    StopAllRunningThreads(m_TIDs);
    WaitThreadStop(g_waitForAllThreads);
    allThreadsWereStopped = true;
}

// In case we need remove breakpoint from address, we must care about all threads first, since some threads could break on this breakpoint already.
// Note, at this point we don't need step over breakpoint, since we don't need "fix, step and restore" logic here.
void InteropDebuggerHelpers::BrkFixAllThreads(std::uintptr_t checkAddr)
{
    for (auto &entry : m_TIDs)
    {
        // get registers (we need PC)
        user_regs_struct regs;
        iovec iov;
        iov.iov_base = &regs;
        iov.iov_len = sizeof(user_regs_struct);
        if (async_ptrace(PTRACE_GETREGSET, entry.first, (void*)NT_PRSTATUS, &iov) == -1)
        {
            LOGW("Ptrace getregset error: %s\n", strerror(errno));
            continue; // Will hope, this thread didn't stopped at breakpoint.
        }

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

static std::array<unw_word_t, UNW_REG_LAST + 1> *InitContextRegs(std::array<unw_word_t, UNW_REG_LAST + 1> &contextRegs, CONTEXT *context)
{
    assert(!!context);

#if defined(UNW_TARGET_X86)
    contextRegs[UNW_X86_EAX]       = context->Eax;
    contextRegs[UNW_X86_EBX]       = context->Ebx;
    contextRegs[UNW_X86_ECX]       = context->Ecx;
    contextRegs[UNW_X86_EDX]       = context->Edx;
    contextRegs[UNW_X86_ESI]       = context->Esi;
    contextRegs[UNW_X86_EDI]       = context->Edi;
    contextRegs[UNW_X86_EBP]       = context->Ebp;
    contextRegs[UNW_X86_EIP]       = context->Eip;
    contextRegs[UNW_X86_ESP]       = context->Esp;
#elif defined(UNW_TARGET_X86_64)
    contextRegs[UNW_X86_64_RAX]    = context->Rax;
    contextRegs[UNW_X86_64_RDX]    = context->Rdx;
    contextRegs[UNW_X86_64_RCX]    = context->Rcx;
    contextRegs[UNW_X86_64_RBX]    = context->Rbx;
    contextRegs[UNW_X86_64_RSI]    = context->Rsi;
    contextRegs[UNW_X86_64_RDI]    = context->Rdi;
    contextRegs[UNW_X86_64_RBP]    = context->Rbp;
    contextRegs[UNW_X86_64_RSP]    = context->Rsp;
    contextRegs[UNW_X86_64_R8]     = context->R8;
    contextRegs[UNW_X86_64_R9]     = context->R9;
    contextRegs[UNW_X86_64_R10]    = context->R10;
    contextRegs[UNW_X86_64_R11]    = context->R11;
    contextRegs[UNW_X86_64_R12]    = context->R12;
    contextRegs[UNW_X86_64_R13]    = context->R13;
    contextRegs[UNW_X86_64_R14]    = context->R14;
    contextRegs[UNW_X86_64_R15]    = context->R15;
    contextRegs[UNW_X86_64_RIP]    = context->Rip;
#elif defined(UNW_TARGET_ARM)
    contextRegs[UNW_ARM_R0]        = context->R0;
    contextRegs[UNW_ARM_R1]        = context->R1;
    contextRegs[UNW_ARM_R2]        = context->R2;
    contextRegs[UNW_ARM_R3]        = context->R3;
    contextRegs[UNW_ARM_R4]        = context->R4;
    contextRegs[UNW_ARM_R5]        = context->R5;
    contextRegs[UNW_ARM_R6]        = context->R6;
    contextRegs[UNW_ARM_R7]        = context->R7;
    contextRegs[UNW_ARM_R8]        = context->R8;
    contextRegs[UNW_ARM_R9]        = context->R9;
    contextRegs[UNW_ARM_R10]       = context->R10;
    contextRegs[UNW_ARM_R11]       = context->R11;
    contextRegs[UNW_ARM_R12]       = context->R12;
    contextRegs[UNW_ARM_R13]       = context->Sp;
    contextRegs[UNW_ARM_R14]       = context->Lr;
    contextRegs[UNW_ARM_R15]       = context->Pc;
#elif defined(UNW_TARGET_AARCH64)
    contextRegs[UNW_AARCH64_X0]       = context->X0;
    contextRegs[UNW_AARCH64_X1]       = context->X1;
    contextRegs[UNW_AARCH64_X2]       = context->X2;
    contextRegs[UNW_AARCH64_X3]       = context->X3;
    contextRegs[UNW_AARCH64_X4]       = context->X4;
    contextRegs[UNW_AARCH64_X5]       = context->X5;
    contextRegs[UNW_AARCH64_X6]       = context->X6;
    contextRegs[UNW_AARCH64_X7]       = context->X7;
    contextRegs[UNW_AARCH64_X8]       = context->X8;
    contextRegs[UNW_AARCH64_X9]       = context->X9;
    contextRegs[UNW_AARCH64_X10]      = context->X10;
    contextRegs[UNW_AARCH64_X11]      = context->X11;
    contextRegs[UNW_AARCH64_X12]      = context->X12;
    contextRegs[UNW_AARCH64_X13]      = context->X13;
    contextRegs[UNW_AARCH64_X14]      = context->X14;
    contextRegs[UNW_AARCH64_X15]      = context->X15;
    contextRegs[UNW_AARCH64_X16]      = context->X16;
    contextRegs[UNW_AARCH64_X17]      = context->X17;
    contextRegs[UNW_AARCH64_X18]      = context->X18;
    contextRegs[UNW_AARCH64_X19]      = context->X19;
    contextRegs[UNW_AARCH64_X20]      = context->X20;
    contextRegs[UNW_AARCH64_X21]      = context->X21;
    contextRegs[UNW_AARCH64_X22]      = context->X22;
    contextRegs[UNW_AARCH64_X23]      = context->X23;
    contextRegs[UNW_AARCH64_X24]      = context->X24;
    contextRegs[UNW_AARCH64_X25]      = context->X25;
    contextRegs[UNW_AARCH64_X26]      = context->X26;
    contextRegs[UNW_AARCH64_X27]      = context->X27;
    contextRegs[UNW_AARCH64_X28]      = context->X28;
    contextRegs[UNW_AARCH64_X29]      = context->Fp;
    contextRegs[UNW_AARCH64_X30]      = context->Lr;
    contextRegs[UNW_AARCH64_SP]       = context->Sp;
    contextRegs[UNW_AARCH64_PC]       = context->Pc;
    contextRegs[UNW_AARCH64_PSTATE]   = context->Cpsr;

#else
#error "Unsupported platform"
#endif

    return &contextRegs;
}

HRESULT InteropDebugger::UnwindNativeFrames(pid_t pid, bool firstFrame, std::uintptr_t endAddr, CONTEXT *pStartContext,
                                            std::function<HRESULT(NativeFrame &nativeFrame)> nativeFramesCallback)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    // Note, user could provide TID with `bt --thread TID` that don't even belong to debuggee process.
    auto tid = m_TIDs.find(pid);
    if (tid == m_TIDs.end())
        return E_INVALIDARG;

    bool threadWasStopped = false;
    if (tid->second.stat == thread_stat_e::running)
    {
        if (async_ptrace(PTRACE_INTERRUPT, pid, nullptr, nullptr) == -1)
            LOGW("Ptrace interrupt error: %s\n", strerror(errno));
        else
        {
            WaitThreadStop(pid);
            threadWasStopped = true;
        }
    }

#if defined(DEBUGGER_UNIX_ARM)
    if (endAddr != 0)
        endAddr = endAddr & ~((std::uintptr_t)1); // convert to proper (even) address (we use only even addresses here for testing and debug info search)
#endif

    // Note, CoreCLR could provide wrong SP in context for some cases, so, we can't use it for find "End" point of unwinding.
    // The main point is - all unwind blocks that we have with `endAddr` provided is "native -> CoreCLR native frame".
    // In case we have `endAddr` and don't reach it during unwind (that usually mean we failed to find CoreCLR native frame address),
    // use first unknown address in unknown memory (that don't belong any native libs) as "End" point.
    // In case we don't have frames with unknown address in unknown memory, just add "[Unknown native frame(s)]" frame at the end.

    bool endAddrReached = false;
    bool unwindTruncated = false;
    static const std::size_t maxFrames = 1000;
    std::vector<std::uintptr_t> addrFrames;
    addrFrames.reserve(maxFrames);

    std::array<unw_word_t, UNW_REG_LAST + 1> contextRegs;
    ThreadStackUnwind(pid, pStartContext ? InitContextRegs(contextRegs, pStartContext) : nullptr, [&](std::uintptr_t addr)
    {

#if defined(DEBUGGER_UNIX_ARM)
        addr = addr & ~((std::uintptr_t)1); // convert to proper (even) address (debug info use only even addresses)
#endif

        if (endAddr != 0 && endAddr == addr)
        {
            endAddrReached = true;
            return false;
        }

        if (addrFrames.size() == maxFrames)
        {
            unwindTruncated = true;
            return false;
        }

        addrFrames.emplace_back(addr);
        return true;
    });

    HRESULT Status = S_OK;
    for (auto addr : addrFrames)
    {
        NativeFrame result;
        result.addr = addr;

        std::uintptr_t libStartAddr = 0;
        std::uintptr_t procStartAddr = 0;
        // Note, in case unwind we need info for address that is part of previous (already executed) code for all frames except first.
        m_uniqueInteropLibraries->FindDataForAddr(firstFrame ? addr : addr - 1, result.libName, libStartAddr, result.procName, procStartAddr, result.fullSourcePath, result.lineNum);
        firstFrame = false;

        if (endAddr != 0 && !endAddrReached && result.libName.empty())
            break;

        std::ostringstream ss;
        if (result.procName.empty()) // in case we can't find procedure name at all - this is "unnamed symbol"
        {
            ss << "unnamed_symbol";
            if (!result.libName.empty() && libStartAddr)
                ss << ", " << result.libName << " + " << addr - libStartAddr;
        }
        else if (result.fullSourcePath.empty()) // in case we have procedure name without code source info - no debug data available (dynsym table data was used)
        {
            ss << result.procName;

            if (procStartAddr)
                ss << " + " << addr - procStartAddr;
        }
        else // we found all data we need from debug info
        {
            ss << result.procName;
        }
        result.procName = ss.str();

        if (FAILED(Status = nativeFramesCallback(result)))
            break;
    }

    // In case we not found frame with end address.
    if (endAddr != 0 && !endAddrReached && SUCCEEDED(Status))
    {
        NativeFrame result;
        result.unknownFrameAddr = true;
        result.procName = "[Unknown native frame(s)]";
        Status = nativeFramesCallback(result);
    }

    // In case unwind was truncated.
    if (unwindTruncated && endAddr == 0 && SUCCEEDED(Status))
    {
        NativeFrame result;
        result.unknownFrameAddr = true;
        result.procName = "Unwind was truncated";
        Status = nativeFramesCallback(result);
    }

    if (threadWasStopped)
        ParseThreadsChanges();

    return Status;
}

HRESULT InteropDebugger::GetFrameForAddr(std::uintptr_t addr, StackFrame &frame)
{
    std::uintptr_t libStartAddr = 0;
    std::uintptr_t procStartAddr = 0;
    std::string libName;
    std::string methodName;
    std::string fullSourcePath;
    int lineNum = 0;
    m_uniqueInteropLibraries->FindDataForAddr(addr, libName, libStartAddr, methodName, procStartAddr, fullSourcePath, lineNum);
    if (methodName.empty())
        methodName = "unnamed_symbol";

    frame.moduleOrLibName = libName;
    frame.methodName = methodName;
    frame.source = Source(fullSourcePath);
    frame.line = lineNum;
    return S_OK;
}

bool InteropDebugger::IsNativeThreadStopped(pid_t pid)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    if (m_TIDs.empty())
        return S_OK;

    auto tid = m_TIDs.find(pid);
    if (tid == m_TIDs.end())
        return E_INVALIDARG;

    assert(tid->second.stat != thread_stat_e::stopped);
    return tid->second.stat != thread_stat_e::running;
}

void InteropDebugger::WalkAllThreads(std::function<void(pid_t, bool)> cb)
{
    std::lock_guard<std::mutex> lock(m_waitpidMutex);

    if (m_TIDs.empty())
        return;

    std::map<pid_t, thread_status_t> orderedTIDs(m_TIDs.begin(), m_TIDs.end());

    for (auto &tid : orderedTIDs)
    {
        cb(tid.first, tid.second.stat == thread_stat_e::running);
    }
}

} // namespace InteropDebugging
} // namespace netcoredbg
