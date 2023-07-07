// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#ifdef INTEROP_DEBUGGING

#include "cor.h"

#include <sys/types.h>
#include <mutex>
#include <thread>
#include <vector>
#include <list>
#include <functional>
#include <condition_variable>
#include <unordered_map>
#include "interfaces/types.h"
#include "debugger/frames.h"

namespace netcoredbg
{

class IProtocol;
class Breakpoints;
class CallbacksQueue;
class EvalWaiter;

namespace InteropDebugging
{

class InteropLibraries;

enum class thread_stat_e
{
    // Important! `stopped` state is temporary and must be parsed by `ParseThreadsChanges()` and not leave `m_waitpidMutex` locked scope.
    // Before leave `m_waitpidMutex` locked scope this state should be reseted to `running` with execution continue or changed to event-related state.
    stopped,
    stopped_on_event_as_native_thread,
    stopped_breakpoint_event_detected,
    stopped_breakpoint_event_in_progress,
    stopped_signal_event_detected,
    stopped_signal_event_in_progress,
    stopped_on_event_need_continue,
    running
};

struct stop_event_data_t
{
    std::uintptr_t addr = 0;
    std::string signal;
};

struct thread_status_t
{
    thread_stat_e stat = thread_stat_e::running;
    unsigned stop_signal = 0;
    unsigned event = 0;

    // Address of breakpoint that single step at "step over breakpoint" was failed.
    // Hold address of breakpoint that must be "step over" (skipped).
    std::uintptr_t addrStepOverBreakpointFailed = 0;

    // Data, that should be stored in order to create stop event (CallbacksQueue) and/or continue thread execution.
    stop_event_data_t stop_event_data;
};

struct callback_event_t
{
    pid_t pid;
    thread_stat_e stat;
    stop_event_data_t stop_event_data;

    callback_event_t(pid_t pid_, thread_stat_e stat_, const stop_event_data_t &data_) :
        pid(pid_),
        stat(stat_),
        stop_event_data(data_)
    {}
};

class InteropDebuggerBase
{
protected:

    InteropDebuggerBase(IProtocol *pProtocol_,
                        std::shared_ptr<Breakpoints> &sharedBreakpoints,
                        std::shared_ptr<EvalWaiter> &sharedEvalWaiter);

    IProtocol *pProtocol;
    std::shared_ptr<Breakpoints> m_sharedBreakpoints;
    std::shared_ptr<CallbacksQueue> m_sharedCallbacksQueue;
    std::unique_ptr<InteropLibraries> m_uniqueInteropLibraries;
    std::shared_ptr<EvalWaiter> m_sharedEvalWaiter;

    enum class WaitpidThreadStatus
    {
        UNKNOWN,
        WORK,
        FINISHED,
        FINISHED_AND_JOINED
    };

    // NOTE we can't setup callbacks in waitpid thread, since CoreCLR could use native breakpoints in managed threads, some managed threads
    //      could be stopped at CoreCLR's breakpoint and wait for waitpid, but we wait for managed process `Stop()` in the same time.
    std::mutex m_callbackEventMutex;
    std::thread m_callbackEventWorker;
    bool m_callbackEventNeedExit = false;
    std::condition_variable m_callbackEventCV;
    std::list<callback_event_t> m_callbackEvents;

    void CallbackEventWorker();

    std::mutex m_waitpidMutex;
    std::thread m_waitpidWorker;
    bool m_waitpidNeedExit = false;
    pid_t m_TGID = 0;
    std::function<void(int)> m_NotifyLastThreadExited;
    std::unordered_map<pid_t, thread_status_t> m_TIDs;
    // We use std::list here, since we need container that not invalidate iterators at `emplace_back()` call.
    std::list<pid_t> m_changedThreads;
    std::vector<pid_t> m_eventedThreads;
    std::condition_variable m_waitpidCV;
    WaitpidThreadStatus m_waitpidThreadStatus = WaitpidThreadStatus::UNKNOWN;

    void InitWaitpidWorkerThread();
    void WaitpidWorker();

    bool m_HWSingleStepSupported = true;

    void WaitThreadStop(pid_t stoppedPid, std::vector<pid_t> *stopTreads = nullptr);
    bool SingleStepOnBrk(pid_t pid, std::uintptr_t addr);
    void ParseThreadsEvents();
    void ParseThreadsChanges();
    virtual void Parse_SIGILL(pid_t pid) = 0;
    virtual void Parse_SIGTRAP__PTRACE_EVENT_EXEC(pid_t pid) = 0;
    virtual void Parse_SIGTRAP__NOT_PTRACE_EVENT(pid_t pid) = 0;
};

class InteropDebuggerSignals : protected InteropDebuggerBase
{
protected:

    InteropDebuggerSignals(IProtocol *pProtocol_,
                           std::shared_ptr<Breakpoints> &sharedBreakpoints,
                           std::shared_ptr<EvalWaiter> &sharedEvalWaiter);

    void Parse_SIGILL(pid_t pid) override;
    void Parse_SIGTRAP__PTRACE_EVENT_EXEC(pid_t pid) override;
    void Parse_SIGTRAP__NOT_PTRACE_EVENT(pid_t pid) override;
};

class InteropDebuggerHelpers : protected InteropDebuggerSignals
{
protected:

    InteropDebuggerHelpers(IProtocol *pProtocol_,
                           std::shared_ptr<Breakpoints> &sharedBreakpoints,
                           std::shared_ptr<EvalWaiter> &sharedEvalWaiter);

    void StopAndDetach(pid_t tgid);
    void Detach(pid_t tgid);

    void LoadLib(pid_t pid, const std::string &libLoadName, const std::string &realLibName, std::uintptr_t startAddr, std::uintptr_t endAddr);
    void UnloadLib(const std::string &realLibName);

    void BrkStopAllThreads(bool &allThreadsWereStopped);
    void BrkFixAllThreads(std::uintptr_t checkAddr);
};

class InteropDebugger final : InteropDebuggerHelpers
{
public:

    InteropDebugger(IProtocol *pProtocol_,
                    std::shared_ptr<Breakpoints> &sharedBreakpoints,
                    std::shared_ptr<EvalWaiter> &sharedEvalWaiter);

    // Initialize interop debugging, attach to process, detect loaded libs, setup native breakpoints, etc.
    HRESULT Init(pid_t pid, std::shared_ptr<CallbacksQueue> &sharedCallbacksQueue, bool attach, std::function<void(int)> NotifyLastThreadExited, int &error_n);
    // Shutdown interop debugging, remove all native breakpoints, detach from threads, etc.
    void Shutdown();

    // Called by CallbacksQueue for continue process (continue threads with processed stop events).
    void ContinueAllThreadsWithEvents();
    HRESULT StopAllNativeThreads(ICorDebugProcess *pProcess);
    HRESULT SetLineBreakpoints(const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT AllBreakpointsActivate(bool act);
    HRESULT BreakpointActivate(uint32_t id, bool act);

    HRESULT GetFrameForAddr(std::uintptr_t addr, StackFrame &frame);
    HRESULT UnwindNativeFrames(pid_t pid, bool firstFrame, std::uintptr_t endAddr, CONTEXT *pStartContext,
                               std::function<HRESULT(NativeFrame &nativeFrame)> nativeFramesCallback);

    bool IsNativeThreadStopped(pid_t pid);
    void WalkAllThreads(std::function<void(pid_t, bool)> cb);
};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
