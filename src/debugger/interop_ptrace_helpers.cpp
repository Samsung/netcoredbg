// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_ptrace_helpers.h"

#include <mutex>
#include <thread>
#include <condition_variable>


namespace netcoredbg
{
namespace InteropDebugging
{

namespace
{

    std::mutex g_ptraceCommandMutex; // prevent more than 1 command execution
    std::mutex g_ptraceMutex; // condition_variable related
    std::condition_variable g_ptraceCV;

    std::thread g_ptraceWorker;
    enum class PtraceThreadStatus
    {
        UNKNOWN,
        WORK,
        FINISHED
    };
    PtraceThreadStatus g_ptraceThreadStatus = PtraceThreadStatus::UNKNOWN;
    bool g_exitPtraceWorker = false;

    struct ptrace_args_t
    {
        __ptrace_request request;
        pid_t pid;
        void *addr;
        void *data;
        void Set(__ptrace_request request_, pid_t pid_, void *addr_, void *data_)
        {
            request = request_;
            pid = pid_;
            addr = addr_;
            data = data_;
        }
    };
    ptrace_args_t g_ptraceArgs;
    long g_ptraceResult = 0;
    int g_errno = 0;

} // unnamed namespace


static void PtraceWorker()
{
    std::unique_lock<std::mutex> lock(g_ptraceMutex);
    g_ptraceCV.notify_one(); // notify async_ptrace_init(), that thread init complete

    while (true)
    {
        g_ptraceCV.wait(lock); // wait for ptrace call request from async_ptrace() or exit request from async_ptrace_shutdown()

        if (g_exitPtraceWorker)
            break;

        errno = 0;
        g_ptraceResult = ptrace(g_ptraceArgs.request, g_ptraceArgs.pid, g_ptraceArgs.addr, g_ptraceArgs.data);
        g_errno = errno;
        g_ptraceCV.notify_one(); // notify async_ptrace(), that result is ready
    }

    g_ptraceCV.notify_one(); // notify async_ptrace_shutdown(), that execution exit from PtraceWorker()
}

void async_ptrace_init()
{
    std::lock_guard<std::mutex> lockCommand(g_ptraceCommandMutex);
    std::unique_lock<std::mutex> lock(g_ptraceMutex);

    if (g_ptraceThreadStatus == PtraceThreadStatus::WORK)
        return;

    g_exitPtraceWorker = false;
    g_ptraceThreadStatus = PtraceThreadStatus::WORK;
    g_ptraceWorker = std::thread(&PtraceWorker);
    g_ptraceCV.wait(lock); // wait for init complete from PtraceWorker
}

void async_ptrace_shutdown()
{
    std::lock_guard<std::mutex> lockCommand(g_ptraceCommandMutex);
    std::unique_lock<std::mutex> lock(g_ptraceMutex);

    if (g_ptraceThreadStatus != PtraceThreadStatus::WORK)
        return;

    g_exitPtraceWorker = true;
    g_ptraceCV.notify_one(); // notify PtraceWorker for exit from infinite loop
    g_ptraceCV.wait(lock); // wait for exit from infinite loop
    g_ptraceThreadStatus = PtraceThreadStatus::FINISHED;
    g_ptraceWorker.join();
}

long async_ptrace(__ptrace_request request, pid_t pid, void *addr, void *data)
{
    std::lock_guard<std::mutex> lockCommand(g_ptraceCommandMutex);
    std::unique_lock<std::mutex> lock(g_ptraceMutex);

    if (g_ptraceThreadStatus != PtraceThreadStatus::WORK)
    {
        errno = EPERM;
        return -1;
    }

    g_ptraceArgs.Set(request, pid, addr, data);

    g_ptraceCV.notify_one(); // notify PtraceWorker for call real ptrace
    g_ptraceCV.wait(lock); // wait for result in g_ptraceResult

    errno = g_errno;
    return g_ptraceResult;
}

} // namespace InteropDebugging
} // namespace netcoredbg
