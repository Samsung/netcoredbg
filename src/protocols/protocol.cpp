// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <algorithm>
#include <vector>
#include <utility>
#include <tuple>
#include <cstddef>
#include <limits>
#include <mutex>
#include "protocol.h"
#include "utility.h"
#include "utils/indexed_storage.h"
#include "metadata/modules.h"  // need GetFileNAme()

namespace netcoredbg
{

// ThreadId == 0 is invalid for Win32 API and PAL library.
/*static*/ const ThreadId ThreadId::Invalid {InvalidValue};

/*static*/ const ThreadId ThreadId::AllThreads {AllThreadsValue};

namespace
{
    struct FramesList
    {
        typedef IndexedStorage<unsigned, std::tuple<ThreadId, FrameLevel> > ListType;

        struct ScopeGuard
        {
            ScopeGuard(FramesList& f) : frames_list(f) { frames_list.mutex.lock(); }

            ~ScopeGuard()  { frames_list.mutex.unlock(); }

            ListType* operator->() const { return &frames_list.list; }

        private:
            FramesList& frames_list;
        };

        ScopeGuard get()
        {
            return *this;
        }

    private:
        std::mutex mutex;
        ListType list;
    };

    // This singleton holds list of frames accessible by index value,
    // this list expires every time when program continues execution.
    typedef Utility::Singleton<FramesList> KnownFrames;
}

FrameId::FrameId(ThreadId thread, FrameLevel level)
: m_id(KnownFrames::instance().get()->emplace(thread, level).first->first)
{
}

FrameId::FrameId(int n) : m_id(n) {}

ThreadId FrameId::getThread() const noexcept
{
    if (*this)
    {
        auto list = KnownFrames::instance().get();
        auto it = list->find(m_id);
        if (it != list->end())
        {
            return std::get<0>(it->second);
        }
    }
    return {};
}


FrameLevel FrameId::getLevel() const noexcept
{
    if (*this)
    {
        auto list = KnownFrames::instance().get();
        auto it = list->find(m_id);
        if (it !=list->end())
        {
            return std::get<1>(it->second);
        }
    }
    return {};
}

/*static*/ void FrameId::invalidate()
{
    KnownFrames::instance().get()->clear();
}


Source::Source(string_view path) : name(Modules::GetFileName(path)), path(path) {}

} // namespace netcoredbg
