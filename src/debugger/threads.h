// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include <set>
#include <vector>
#include "protocols/protocol.h"

namespace netcoredbg
{

ThreadId getThreadId(ICorDebugThread *pThread);

class Threads
{
    std::mutex m_userThreadsMutex;
    std::set<ThreadId> m_userThreads;

public:

    void Add(const ThreadId &threadId);
    void Remove(const ThreadId &threadId);
    HRESULT GetThreadsWithState(ICorDebugProcess *pProcess, std::vector<Thread> &threads);
};

} // namespace netcoredbg
