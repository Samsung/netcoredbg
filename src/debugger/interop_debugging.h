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
#include <condition_variable>
#include <unordered_map>
#include "interfaces/types.h"


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
    stopped,
    stopped_breakpoint_event_detected,
    stopped_breakpoint_event_in_progress,
    running
};

struct thread_status_t
{
    thread_stat_e stat = thread_stat_e::running;
    unsigned int stop_signal = 0;
    unsigned event = 0;

    // Data, that should be stored in order to create stop event (CallbacksQueue) and/or continue thread execution.
    struct
    {
        std::uintptr_t addr = 0;
    } stop_event_data;
};

class InteropDebugger
{
public:

    InteropDebugger(std::shared_ptr<IProtocol> &sharedProtocol,
                    std::shared_ptr<Breakpoints> &sharedBreakpoints,
                    std::shared_ptr<EvalWaiter> &sharedEvalWaiter);

    // Initialize interop debugging, attach to process, detect loaded libs, setup native breakpoints, etc.
    HRESULT Init(pid_t pid, std::shared_ptr<CallbacksQueue> &sharedCallbacksQueue, int &error_n);
    // Shutdown interop debugging, remove all native breakpoints, detach from threads, etc.
    void Shutdown();

    // Called by CallbacksQueue for continue process (continue threads with processed stop events).
    void ContinueAllThreadsWithEvents();
    HRESULT SetLineBreakpoints(const std::string& filename, const std::vector<LineBreakpoint> &lineBreakpoints, std::vector<Breakpoint> &breakpoints);
    HRESULT AllBreakpointsActivate(bool act);
    HRESULT BreakpointActivate(uint32_t id, bool act);

private:

    std::shared_ptr<IProtocol> m_sharedProtocol;
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

    std::mutex m_waitpidMutex;
    std::thread m_waitpidWorker;
    bool m_waitpidNeedExit = false;
    pid_t m_TGID = 0;
    std::unordered_map<pid_t, thread_status_t> m_TIDs;
    // We use std::list here, since we need container that not invalidate iterators at `emplace_back()` call.
    std::list<pid_t> m_changedThreads;
    std::vector<pid_t> m_eventedThreads;
    std::condition_variable m_waitpidCV;
    WaitpidThreadStatus m_waitpidThreadStatus = WaitpidThreadStatus::UNKNOWN;

    void WaitpidWorker();

    void LoadLib(pid_t pid, const std::string &realLibName, std::uintptr_t startAddr, std::uintptr_t endAddr);
    void UnloadLib(const std::string &realLibName);

    void StopAllRunningThreads();
    void WaitAllThreadsStop();
    void StopAndDetach(pid_t tgid);
    void Detach(pid_t tgid);
    void ParseThreadsChanges();
    void ParseThreadsEvents();
    void BrkStopAllThreads(bool &allThreadsWereStopped);
    void BrkFixAllThreads(std::uintptr_t checkAddr);

};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
