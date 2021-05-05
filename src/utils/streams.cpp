// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file streams.cpp  This file contains member definitions of following classes:
/// InStreamBuf, OutStreamBuf, StreamBuf, InStream, OutStream, IOStream.

#include <cstring>
#include <cassert>
#include "utils/limits.h"

#include <algorithm>
#include <thread>

#include "utils/streams.h"

namespace netcoredbg
{

// These constant define default size of the buffer, which typically can hold few lines of the text.
const size_t InStreamBuf::DefaultBufferSize  = 2*LINE_MAX;
const size_t OutStreamBuf::DefaultBufferSize = 2*LINE_MAX;
const size_t StreamBuf::DefaultBufferSize    = 2*LINE_MAX;

namespace
{
    const size_t UngetChars = 1;              // number of extra chars in buffer for ungetting
    const size_t OverflowChars = 1;           // number of extra chars in buffer for overflow

    // minimal buffer sizes for input/output (1 char is minimal buffer size)
    const size_t InputMinBuf = UngetChars + 1;
    const size_t OutputMinBuf = OverflowChars + 1;

    // constants for InStreamBuf
    const static size_t MaxMoveSize = sizeof(void*) * 4;
}


// Arguments are following: `fh` -- file descriptor opened for reading,
// buf_size -- the size of the input buffer.
InStreamBuf::InStreamBuf(const FileHandle& fh, size_t buf_size)
: FileOwner(fh),
  inbuf(std::max(buf_size, InputMinBuf))
{
    setg(inbuf.data(), inbuf.data() + UngetChars, inbuf.data() + UngetChars);

    assert(gptr() == egptr());
}

void InStreamBuf::setegptr(char* egptr)
{
    assert(egptr <= endp() && egptr >= gptr());
    setg(eback(), gptr(), egptr);
}

size_t InStreamBuf::min_read_size() const
{
    return std::max(size_t(endp() - eback()) / 4, size_t(LINE_MAX));
}

void InStreamBuf::compactify()
{
    assert(egptr() >= gptr());
    assert(egptr() >= eback());
    size_t free = endp() - egptr();

    // if some relatively small portion of unread data occupies tail of the 
    // buffer -- move it to the beginning of the buffer to allow reading more
    // data from the file.
    if (free < min_read_size())
    {
        if (size_t(in_avail()) <= MaxMoveSize)   // tail is not too big
        {
            memmove(eback() + UngetChars, gptr(), in_avail());
            setg(eback(), eback() + UngetChars, eback() + UngetChars + in_avail());
        }
    }
}

// This functions fills input buffer (it should made at least one character be
// available in the buffer). Return value is traits_type::eof() in case of error,
// or the code of next available symbol.
int InStreamBuf::underflow()
{
    compactify();

    size_t free = endp() - egptr();
    if (free < min_read_size() && in_avail() > 0)
        return traits_type::to_int_type(*gptr());

    using IOResult = IOSystem::IOResult;
    IOResult res;
    while (true)
    {
        res = IOSystem::read(file_handle, egptr(), free);
        switch (res.status)
        {
        case IOResult::Error:
        case IOResult::Eof:
            return traits_type::eof();

        default: break;  // at least one byte was read
        }

        if (res.status == IOResult::Success)
            break;

        std::this_thread::yield();  // loop  for non-blocking streams
    }

    assert(res.size > 0);
    setg(eback(), gptr(), egptr() + res.size);
    return traits_type::to_int_type(*gptr());
}


// Arguments are following: `fh` -- file descriptor opened for writing,
// buf_size -- the size of the output buffer.
OutStreamBuf::OutStreamBuf(const FileHandle& fh, size_t buf_size)
: FileOwner(fh),
  outbuf(std::max(buf_size, OutputMinBuf))
{
    buf_size = std::max(buf_size, OutputMinBuf);  // TODO govnokod
    setp(outbuf.data(), outbuf.data() + outbuf.size() - OverflowChars);

    assert(pptr() == pbase());
    assert(size_t(epptr() - pbase() + OverflowChars) == buf_size);
}

// Function writes data from a buffer to the file. Function ensures, that
// after return there is space in the buffer for at least one character.
// Function returns Traits::eof() on failure (write error).
int OutStreamBuf::overflow(int c)
{
    if (!traits_type::eq_int_type(c, traits_type::eof()))
    {
        *pptr() = traits_type::to_char_type(c);
        pbump(1);  // need extra char at buffer
    }

    using IOResult = IOSystem::IOResult;

    size_t size;
    IOResult res;
    while (true)
    {
        // try to write data
        size = pptr() - pbase();
        res = IOSystem::write(file_handle, pbase(), size);
        if (res.status == IOResult::Error)
            return traits_type::eof();

        if (res.status == IOResult::Success)
            break;

        std::this_thread::yield();      // for non-blocking streams
     }

    // move unwritten part to the beginning of the buffer
    // left == 0 for blocking streams
    assert(res.size > 0);
    size_t left = size - res.size;
    memmove(pbase(), pbase() + res.size, left);
    setp(pbase(), epptr());
    pbump(int(left));
    return traits_type::not_eof(c);
}


// Function flushes the buffer to the underlying file 
int OutStreamBuf::sync()
{
    while (true)
    {
        // try to write data
        size_t size = pptr() - pbase();
        if (size == 0)
            break;

        using IOResult = IOSystem::IOResult;
        IOResult res = IOSystem::write(file_handle, pbase(), size);
        if (res.status == IOResult::Error)
            return -1;

        if (res.status == IOResult::Success && size == res.size)
           break;   // all data written 

        setp(pbase() + res.size, epptr());
        pbump(int(size - res.size));

        std::this_thread::yield();      // for non-blocking streams        
    }

    setp(outbuf.data(), epptr());
    return 0;
}

}  // ::netcoredbg
