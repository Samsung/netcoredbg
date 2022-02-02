// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#pragma once
#include "utils/utility.h"
#include <mutex>
#include <condition_variable>

namespace netcoredbg
{

namespace Utility
{

class RWLock
{
private:
    template <typename T> struct LockBase
    {
        LockBase() {}
        LockBase(const LockBase&) = delete;
        LockBase& operator=(const LockBase&) = delete;

        template <typename Member> RWLock* rwlock(const Member RWLock::*mem)
        {
            return Utility::container_of(static_cast<T*>(this), mem);
        }
    };

public:
    class Reader : LockBase<Reader>
    {
        friend class RWLock;
        Reader() {}

    public:
        void lock()     { rwlock(&RWLock::reader)->read_lock(); }

        bool try_lock() { return rwlock(&RWLock::reader)->read_try_lock(); }

        void unlock()   { rwlock(&RWLock::reader)->read_unlock(); }
    };

    struct Writer : LockBase<Writer>
    {
        friend class RWLock;
        Writer() {}

    public:
        void lock()     { rwlock(&RWLock::writer)->write_lock(); }

        bool try_lock() { return rwlock(&RWLock::writer)->write_try_lock(); }

        void unlock()   { rwlock(&RWLock::writer)->write_unlock(); }
    };

    Reader reader;
    Writer writer;

    RWLock() : nreaders(0), nwriters(0), is_writing(false) {}

private:
    std::mutex m;
    std::condition_variable cv;
    unsigned nreaders;
    unsigned nwriters;
    bool is_writing;

    void read_lock()
    {
        std::unique_lock<std::mutex> lock(m);

        while (nwriters || is_writing)
            cv.wait(lock);

        ++nreaders;
    }

    bool read_try_lock()
    {
        std::unique_lock<std::mutex> lock(m);

        if (nwriters || is_writing)
            return false;

        ++nreaders;
        return true;
    }

    void read_unlock()
    {
        std::unique_lock<std::mutex> lock(m);

        if (!--nreaders)
            cv.notify_all();
    }

    void write_lock()
    {
        std::unique_lock<std::mutex> lock(m);

        ++nwriters;
        while (nreaders || is_writing)
            cv.wait(lock);

        --nwriters;
        is_writing = true;
    }

    bool write_try_lock()
    {
        std::unique_lock<std::mutex> lock(m);

        if (nreaders || is_writing)
            return false;

        is_writing = true;
        return true;
    }

    void write_unlock()
    {
        std::unique_lock<std::mutex> lock(m);

        is_writing = false;
        cv.notify_all();
    }
};

}
}


