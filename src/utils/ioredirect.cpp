// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file ioredirect.h
/// This file contains definitions of`IORedirectHelper` class members.

#include <string.h>
#include "utils/limits.h"
#include "utils/streams.h"
#include "utils/ioredirect.h"
#include "interfaces/idebugger.h"
#include "utils/logger.h"
#include "utils/rwlock.h"

namespace netcoredbg
{

// This constant represents default buffers size for input/output.
// Typically buffer with default size can hold few lines of text.
const size_t IORedirectHelper::DefaultBufferSize = 2*LINE_MAX;

namespace
{
    // timeout for select() call
    std::chrono::milliseconds WaitForever{INT_MAX / 1000};

    char *get_streams_pptr(std::tuple<OutStream, InStream, InStream> &m_streams)
    {
        auto *out = dynamic_cast<OutStreamBuf*>(std::get<IOSystem::Stdin>(m_streams).rdbuf());
        if (out == nullptr)
        {
            LOGE("dynamic_cast fail");
            return nullptr;
        }
        return out->pptr();
    }
}


IORedirectHelper::IORedirectHelper(
    const Pipes &pipes,             // three pair of pipes which represent stdin/stdout/sterr files
    const InputCallback &callback,  // callback functor, which is called when some data became available
    size_t input_bufsize,           // input buffer size (for stdou/stderr streams)
    size_t output_bufsize           // output buffer size (for stdin stream)
)
: m_pipes {
    std::get<IOSystem::Stdin>(pipes).first,
    std::get<IOSystem::Stdout>(pipes).second,
    std::get<IOSystem::Stderr>(pipes).second
  },
  m_streams {
    OutStream(OutStreamBuf(std::get<IOSystem::Stdin>(pipes).second, output_bufsize)),
    InStream(InStreamBuf(std::get<IOSystem::Stdout>(pipes).first, input_bufsize)),
    InStream(InStreamBuf(std::get<IOSystem::Stderr>(pipes).first, input_bufsize))
  },
  m_callback(callback),
  m_sent(get_streams_pptr(m_streams)),
  m_unsent(m_sent),
  m_eof(),
  m_worker_pipe(IOSystem::unnamed_pipe()),
  m_input_pipe(IOSystem::unnamed_pipe()),
  m_cancel(),
  m_finish(),
  m_thread{&IORedirectHelper::worker, this}
{
    assert(std::get<IOSystem::Stdin>(pipes).first);
    assert(std::get<IOSystem::Stdin>(pipes).second);
    assert(std::get<IOSystem::Stdout>(pipes).first);
    assert(std::get<IOSystem::Stdout>(pipes).second);
    assert(std::get<IOSystem::Stderr>(pipes).first);
    assert(std::get<IOSystem::Stderr>(pipes).second);

    assert(m_sent != nullptr);

    // prohibit inheritance of "our" pipe ends
    IOSystem::set_inherit(std::get<IOSystem::Stdin>(pipes).second, false);
    IOSystem::set_inherit(std::get<IOSystem::Stdout>(pipes).first, false);
    IOSystem::set_inherit( std::get<IOSystem::Stderr>(pipes).first, false);

    // enable inheritance of "remote" pipe ends
    IOSystem::set_inherit(std::get<IOSystem::Stdin>(pipes).first, true);
    IOSystem::set_inherit(std::get<IOSystem::Stdout>(pipes).second, true);
    IOSystem::set_inherit(std::get<IOSystem::Stderr>(pipes).second, true);
}

IORedirectHelper::~IORedirectHelper()
{
    LOGD("request worker to exit");
    m_finish = true;  // signal worker thread to stop
    wake_worker();
    m_thread.join();
}


void IORedirectHelper::wake_worker()
{
    LOGD("waking worker");
    IOSystem::write(m_worker_pipe.second, "", 1);
}

void IORedirectHelper::wake_reader()
{
    LOGD("waking reader");
    IOSystem::write(m_input_pipe.second, "", 1);
}


// Output buffer management:
// ----------SSSSSSSSSSS++++++++++----------
// ^         ^          ^         ^        ^
// pbase()   m_sent     m_unsent  pptr()   epptr()
//
// legend:
//    - free space (already sent data or the left)
//    S segment of data for which IO request is in progress
//    + unwritten data for which IO request isn't issued
//
// New data might be written in buffer (with locked mutex)
// starting from pptr() till epptr(). overflow() shold never
// be called (because it conflicts with asynchronous write).


// Worker thread function: this function monitors input pipes, which corresponds
// to stdout/stderr streams, and call callback functor when data received.
void IORedirectHelper::worker()
{
    static constexpr StreamType stream_types[] = {
        IOSystem::Stdin, IOSystem::Stdout, IOSystem::Stderr };

    InStreamBuf* const in_streams[] = {
        nullptr, 
        dynamic_cast<InStreamBuf*>(std::get<stream_types[1]>(m_streams).rdbuf()),
        dynamic_cast<InStreamBuf*>(std::get<stream_types[2]>(m_streams).rdbuf()) };

    OutStreamBuf* const out_stream = 
        dynamic_cast<OutStreamBuf*>(std::get<stream_types[0]>(m_streams).rdbuf());
    if (out_stream == nullptr)
    {
        LOGE("dynamic_cast fail");
        return;
    }

    // currently existing asyncchronous io requests
    IOSystem::AsyncHandle async_handles[Utility::Size(stream_types) + 1];
    auto& out_handle = async_handles[0];
    auto& pipe_handle = async_handles[Utility::Size(stream_types)];

    // at exit: cancel all unfinished io requests
    auto on_exit = [&](void *)
    {
        for (auto& h : async_handles)
            if (h) IOSystem::async_cancel(h);

        LOGI("IORedirectHelper::worker: terminated");
    };

    std::unique_ptr<void, decltype(on_exit)> catch_exit {this, on_exit};

    // issue read request for control pipe
    char dummybuf;
    pipe_handle = IOSystem::async_read(m_worker_pipe.first, &dummybuf, 1);
    assert(pipe_handle);

    LOGI("%s started", __func__);

    std::unique_lock<Utility::RWLock::Reader> read_lock(m_rwlock.reader, std::defer_lock_t{});

    // loop till fatal error or exit request
    while (true)
    {
        // start new write requests if possible
        if (!out_handle)
            StartNewWriteRequests(read_lock, out_stream, out_handle);

        // process data available in buffer for input in_streams
        for (unsigned n = 0; n < Utility::Size(stream_types); n++)
        {
            InStreamBuf* const stream = in_streams[n];
            if (stream == nullptr)
                continue;

            // process data already existing in the buffer
            size_t avail = stream->egptr() - stream->gptr();
            if (avail)
            {
                LOGD("push %u bytes to callback", int(avail));
                m_callback(stream_types[n], span<char>(stream->gptr(), avail));
                stream->gbump(int(avail));
                stream->compactify();
            }

            // request to read more data
            if (!async_handles[n])
            {
                size_t free_size = stream->endp() - stream->egptr();
                LOGD("requesting %u bytes to read", int(free_size));
                async_handles[n] =
                    IOSystem::async_read(stream->get_file_handle(), stream->gptr(), free_size);

                if (LOGE_IF(!async_handles[n], "can't issue async read request!"))
                    return;
            }
        }

        // check if data available for reading or write operation is finished
        IOSystem::async_wait(async_handles, &async_handles[Utility::Size(async_handles)], WaitForever);
        LOGD("%s: wake", __func__);

        // check if termination requested
        IOSystem::IOResult result = IOSystem::async_result(pipe_handle);
        if (result.status != IOSystem::IOResult::Pending)
        {
            if (LOGE_IF(result.status != IOSystem::IOResult::Success, "control pipe read error"))
                return;

            if (m_finish)
                return;    // exit request

            // issue next read request for control pipe
            pipe_handle = IOSystem::async_read(m_worker_pipe.first, &dummybuf, 1);
        }

        // process finished write requests
        if (out_handle &&
            !ProcessFinishedWriteRequests(read_lock, out_stream, out_handle))
            return;

        // process finished read requests
        if (!ProcessFinishedReadRequests(in_streams, Utility::Size(stream_types), async_handles))
            return;
    }
}

void IORedirectHelper::StartNewWriteRequests(std::unique_lock<Utility::RWLock::Reader> &read_lock, OutStreamBuf* const out_stream, IOSystem::AsyncHandle &out_handle)
{
    assert(!read_lock);
    read_lock.lock();

    assert(out_stream->pbase() <= m_sent && m_sent <= m_unsent
            && m_unsent <= out_stream->pptr() && out_stream->pptr() <= out_stream->epptr());

    size_t bytes = out_stream->pptr() - m_unsent;
    if (bytes)
    {
        LOGD("have %u bytes unsent", int(bytes));
        out_handle =
            IOSystem::async_write(out_stream->get_file_handle(), m_unsent, bytes);

        if (LOGE_IF(!out_handle, "can't issue async write request!"))
            return;

        m_unsent = out_stream->pptr();
    }
    else 
    {
        // close writing end of debugee's stdin pipe if
        // we reached EOF on reading input from user and
        // if all previously received data is completely sent.
        if (m_eof)
        {
            LOGD("closing writing end of stdin's pipe");
            auto forgetme = std::move(dynamic_cast<OutStream&>(std::get<IOSystem::Stdin>(m_streams)));
        }

        read_lock.unlock();
    }
}

bool IORedirectHelper::ProcessFinishedWriteRequests(std::unique_lock<Utility::RWLock::Reader> &read_lock, OutStreamBuf* const out_stream, IOSystem::AsyncHandle &out_handle)
{
    IOSystem::IOResult result = IOSystem::async_result(out_handle);
    if (result.status == IOSystem::IOResult::Success)
    {
        // update buffer
        assert(read_lock);
        assert(out_stream->pbase() <= m_sent && m_sent <= m_unsent
                && m_unsent <= out_stream->pptr() && out_stream->pptr() <= out_stream->epptr());

        LOGD("sent %u bytes", int(result.size));
        assert(result.size <= size_t(m_unsent - m_sent));
        m_sent += result.size;

        out_handle = {};  // can issue next read request

        read_lock.unlock();

        // process situation, when end of buffer reached.
        if (m_rwlock.writer.try_lock())
        {
            bool updated = false;

            // can move tail to beginning of the buffer
            size_t bytes = out_stream->pptr() - m_unsent; // num of unsent bytes
            if (m_unsent == m_sent && bytes == 0)
            {
                memmove(out_stream->pbase(), m_unsent, bytes);
                m_sent = m_unsent = out_stream->pbase();
                out_stream->clear();
                out_stream->pbump(int(bytes));
                
                updated = true;
            }

            m_rwlock.writer.unlock();

            // wake reader to read more data
            if (updated)
                wake_reader();
        }
    }
    else if (result.status != IOSystem::IOResult::Pending)
    {   // fatal error
        out_handle = {};
        LOGE("child process stdin writing error");
        return false;
    }

    return true;
}

bool IORedirectHelper::ProcessFinishedReadRequests(InStreamBuf* const in_streams[], size_t stream_types_cout, IOSystem::AsyncHandle async_handles[])
{
    for (size_t n = 0; n < stream_types_cout; n++)
    {
        InStreamBuf* const stream = in_streams[n];
        if (stream == nullptr)
            continue;

        IOSystem::IOResult result = IOSystem::async_result(async_handles[n]);
        if (result.status == IOSystem::IOResult::Success)
        {
            // update buffer
            LOGD("read %u bytes", int(result.size));
            assert(result.size <= size_t(stream->endp() - stream->gptr()));
            stream->setegptr(stream->egptr() + result.size);

            async_handles[n] = {};  // can issue next read request
        }
        else if (result.status != IOSystem::IOResult::Pending)
        {   // fatal error
            async_handles[n] = {};
            LOGE("child process stdout/stderr reading error");
            return false;
        }
    }

    return true;
}


void IORedirectHelper::async_cancel()
{
    LOGD("canceling reading of real stdin");
    bool expected = false;
    if (m_cancel.compare_exchange_strong(expected, true))
        wake_reader();
}



IDebugger::AsyncResult IORedirectHelper::async_input(InStream& in)
{
    if (m_eof)
        return AsyncResult::Eof;

    auto *out = dynamic_cast<OutStreamBuf*>(std::get<IOSystem::Stdin>(m_streams).rdbuf());
    if (out == nullptr)
    {
        LOGE("dynamic_cast fail");
        return AsyncResult::Error;
    }

    IOSystem::AsyncHandle async_handles[2];
    auto& input_handle = async_handles[0];
    auto& pipe_handle = async_handles[1];

    auto on_exit = [&](void *)
    {
        for (auto& h : async_handles)
            if (h) IOSystem::async_cancel(h);

        bool expected = true;
        if (m_cancel.compare_exchange_strong(expected, false))
            LOGD("async_input: canceled");
    };

    std::unique_ptr<void, decltype(on_exit)> catch_exit {this, on_exit};

    // issue read request for control pipe
    char dummybuf;
    pipe_handle = IOSystem::async_read(m_input_pipe.first, &dummybuf, 1);
    if (LOGE_IF(!pipe_handle, "%s: control pipe reading error", __func__))
        return AsyncResult::Error;

    std::unique_lock<Utility::RWLock::Reader> read_lock(m_rwlock.reader, std::defer_lock_t{});

    // loop until termination request
    LOGD("async_input: entering in loop");
    while (true)
    {
        // issue new read request if no async read performed currenly
        if (!input_handle && !m_eof)
        {
            assert(!read_lock);
            read_lock.lock();
            assert(out->pbase() <= out->pptr() && out->pptr() <= out->epptr());

            // free bytes in output buffer
            size_t avail = out->epptr() - out->pptr();
            if (avail)
            {
                LOGD("requesting %u bytes to read", int(avail));
                input_handle =
                    IOSystem::async_read(in.get_file_handle(), out->pptr(), avail);

                if (LOGE_IF(!input_handle, "can't issue read request for real stdin"))
                    return AsyncResult::Error;
            }
            else {
                read_lock.unlock();
            }
        }

        // wait for finish of read request with small timeout
        // NOTE: using here polling, but not waiting, on Win32,
        // to avoid issue with blocking console read operation...
#ifdef _WIN32
        const std::chrono::milliseconds PollPeriod{100};
#else
        auto& PollPeriod = WaitForever;
#endif
        if (IOSystem::async_wait(async_handles, &async_handles[Utility::Size(async_handles)], PollPeriod))
            LOGD("%s: wake", __func__);

        // check if cancellation requested
        IOSystem::IOResult result = IOSystem::async_result(pipe_handle);
        if (result.status != IOSystem::IOResult::Pending)
        {
            if (LOGE_IF(result.status != IOSystem::IOResult::Success, "control pipe read error"))
                return AsyncResult::Error;

            if (m_cancel.load())
                return AsyncResult::Canceled;

            pipe_handle = IOSystem::async_read(m_input_pipe.first, &dummybuf, 1);
        }

        // check if asynchronous read request finished
        if (input_handle)
        {
            result = IOSystem::async_result(input_handle);
            if (result.status == IOSystem::IOResult::Success)
            {
                input_handle = {};  // can issue next read request

                // some data received
                assert(read_lock);
                assert(out->pbase() <= out->pptr() && out->pptr() <= out->epptr());

                LOGD("read %u bytes from stdin", int(result.size));
                assert(result.size <= size_t(out->epptr() - out->pptr()));
                out->pbump(int(result.size));

                read_lock.unlock();
                wake_worker();  // worker must be waked after modifying `out` buffer
            }
            else if (result.status == IOSystem::IOResult::Eof)
            {
                // instruct worker to close writing end of pipe
                LOGD("EOF reached");
                m_eof = true;
                wake_worker();
                return AsyncResult::Eof;
            }
            else if (result.status == IOSystem::IOResult::Error)
            {   // fatal error
                input_handle = {};
                LOGE("real stdin read error");
                return AsyncResult::Canceled;
            }
        }
    }
}


} // ::netcoredbg
