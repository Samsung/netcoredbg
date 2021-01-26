// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file ioredirect.h
/// This file contains definitions of`IORedirectHelper` class members.

#include "limits.h"
#include "streams.h"
#include "ioredirect.h"

namespace netcoredbg
{

// This constant represents default buffers size for input/output.
// Typically buffer with default size can hold few lines of text.
const size_t IORedirectHelper::DefaultBufferSize = 2*LINE_MAX;

namespace
{
    // timeout for select() call
    std::chrono::milliseconds MaxWait{200};
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
  m_thread{&IORedirectHelper::worker, this},
  m_finish{false}
{
    assert(std::get<IOSystem::Stdin>(pipes).first);
    assert(std::get<IOSystem::Stdin>(pipes).second);
    assert(std::get<IOSystem::Stdout>(pipes).first);
    assert(std::get<IOSystem::Stdout>(pipes).second);
    assert(std::get<IOSystem::Stderr>(pipes).first);
    assert(std::get<IOSystem::Stderr>(pipes).second);

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
    m_finish = true;    // signal worker thread to stop
    m_thread.join();
}


// This function allows to write some data to pipe, which represents stdin stream.
void IORedirectHelper::output(const char *data, size_t size)
{
    auto& out = std::get<IOSystem::Stdin>(m_streams);
    out.write(data, size);
    out.flush();
}


// Worker thread function: this function monitors input pipes, which corresponds
// to stdout/stderr streams, and call callback functor when data received.
void IORedirectHelper::worker()
{
    static constexpr StreamType stream_types[] = { IOSystem::Stdout, IOSystem::Stderr };
    static const unsigned NStreams = sizeof(stream_types)/sizeof(stream_types[0]);

    InStreamBuf* const streams[NStreams] = {
            dynamic_cast<InStreamBuf*>(std::get<stream_types[0]>(m_streams).rdbuf()),
            dynamic_cast<InStreamBuf*>(std::get<stream_types[1]>(m_streams).rdbuf()) };

    IOSystem::AsyncHandle async_handles[NStreams];
    while (!m_finish)
    {
        for (unsigned n = 0; n < NStreams; n++)
        {
            InStreamBuf* const stream = streams[n];

            // process data already existing in the buffer
            size_t avail = stream->egptr() - stream->gptr();
            if (avail)
            {
                m_callback(stream_types[n], span<char>(stream->gptr(), avail));
                stream->gbump(int(avail));
                stream->compactify();
            }

            // request to read more data
            if (!async_handles[n])
            {
                async_handles[n] = IOSystem::async_read(
                                    stream->get_file_handle(),
                                    stream->gptr(),
                                    stream->endp() - stream->egptr());

                if (!async_handles[n])
                    m_finish = 1;
            }
        }

        if (m_finish)
            break;

        // check if data available for reading?
        if (!IOSystem::async_wait(async_handles, &async_handles[NStreams], MaxWait))
            continue;

        for (unsigned n = 0; n < NStreams; n++)
        {
            InStreamBuf* const stream = streams[n];

            IOSystem::IOResult result = IOSystem::async_result(async_handles[n]);
            if (result.status == IOSystem::IOResult::Pending)
            {
                continue;
            }
            else if (result.status == IOSystem::IOResult::Success)
            {
                // update buffer
                assert(result.size <= size_t(stream->endp() - stream->gptr()));
                stream->setegptr(stream->egptr() + result.size);

                // issue next read request
                async_handles[n] = IOSystem::AsyncHandle();
            }
            else
            {   // fatal error
                async_handles[n] = IOSystem::AsyncHandle();
                m_finish = 1;
                break;
            }
        }
    }

    for (unsigned n = 0; n < NStreams; n++)
    {
        if (async_handles[n])
            IOSystem::async_cancel(async_handles[n]);
    }
}

} // ::netcoredbg
