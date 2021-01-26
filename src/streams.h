// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file streams.h  This file contains declaration of few classes implementing
/// std::streambuf and std::iostream interfaces.

#pragma once
#include <cstddef>
#include <cstdint>
#include <streambuf>
#include <iostream>
#include <vector>
#include "iosystem.h"

namespace netcoredbg
{

// Classes not designated for public use.
namespace StreamsInternal
{
    // This class isn't intendent for direct use and is used by InStreamBuf,
    // OutStreamBuf and StreamBuf classes.  This class allows to hold ownership
    // of open file during its lifetime and to close file on destruction.
    class FileOwner
    {
    public:
        using FileHandle = IOSystem::FileHandle;

        FileHandle get_file_handle() const { return file_handle; }

    protected:
        FileOwner(const FileHandle& fh) : file_handle(fh) {}

        FileOwner(const FileOwner&) = delete;

        FileOwner(FileOwner&& other) noexcept : file_handle(other.file_handle)
        {
            other.file_handle = FileHandle();
        }

        ~FileOwner()
        {
            if (file_handle)
                IOSystem::close(file_handle);
        }

        FileHandle file_handle;
    };

    // This class isn't intendent for direct use and is used by InStreamBuf,
    // OutStreamBuf and StreamBuf classes.  This class allows to hold ownership
    // of open file during its lifetime and to close file on destruction.
    template <typename T>
    struct BufferOwner
    {
        T buffer;
    };
}


/// This class implements `std::streambuf` interface for input-only data streams.
///
/// Class owns opened file descriptor and operates on this to read data.
///
/// This class exposes few private for `std::streambuf` functions (`underflow`,
/// `gptr`, `egptr` and `gbump`) which allows to read and process data
/// in place, without copying.
///
class InStreamBuf : virtual StreamsInternal::FileOwner, public virtual std::streambuf
{
public:
    using FileHandle = IOSystem::FileHandle;
    using traits_type = std::streambuf::traits_type;

    /// This constant defines default size of input buffer, which typically
    /// can hold few lines of the text.
    static const size_t DefaultBufferSize;

    /// Arguments are following: `fh` -- file descriptor opened for reading,
    /// buf_size -- the size of the input buffer.
    InStreamBuf(const FileHandle& fh, size_t buf_size = DefaultBufferSize);

    /// Class isn't copyable.
    InStreamBuf(const InStreamBuf&) = delete;

    /// Class is movable.
    InStreamBuf(InStreamBuf&& other) noexcept
        : FileOwner(std::move(other)), std::streambuf(other), inbuf(std::move(other.inbuf)) {}

    virtual ~InStreamBuf() {}

    /// Function returns file handle which is used for writing data.
    FileHandle get_file_handle() const { return FileOwner::get_file_handle(); }

    // functions which should be implemented for std::streambuf
    // following functions made public to available implementation of IORedirect class
public:
    /// This functions fills input buffer (it should made at least one character be
    /// available in the buffer). Return value is traits_type::eof() in case of error,
    /// or the code of next available symbol.
    virtual int underflow() override;

    /// Function returns pointer to the next available character.
    char* gptr() const { return std::streambuf::gptr(); }

    /// Function returns the pointer one past the end of the buffer.
    char* egptr() const { return std::streambuf::egptr(); }

    /// Function advances the next pointer in the input sequence.
    void gbump(int count) { return std::streambuf::gbump(count); }

    /// Function returns pointer beyond last byte of the buffer.
    char* endp() const { return eback() + inbuf.size(); }

    /// Functions sets new egptr value (after reading new data into buffer, for example),
    /// egptr should be less or equal to endp().
    void setegptr(char *egptr);

    /// This function moves tail of the buffer to the beginning, creating more free space.
    void compactify();

private:
    size_t min_read_size() const;

    std::vector<char> inbuf; // underlying storage for the input buffer
};


/// This class implements `std::streambuf` interface for write-only data streams.
///
/// Class owns opened file descriptor and operates on this to write data.
///
/// This class exposes few private for `std::streambuf` functions (`overflow`,
/// `pptr`, `epptr` and `pbump`) which allows to generate data directly
/// in the buffer and avoid unnecessary copying.
///
class OutStreamBuf : virtual StreamsInternal::FileOwner, public virtual std::streambuf
{
public:
    using FileHandle = IOSystem::FileHandle;
    using traits_type = std::streambuf::traits_type;

    /// This constant defines default size of input buffer, which typically
    /// can hold few lines of the text.
    const static size_t DefaultBufferSize;

    /// Arguments are following: `fh` -- file descriptor opened for writing,
    /// buf_size -- the size of the output buffer.
    OutStreamBuf(const FileHandle& fh, size_t buf_size = DefaultBufferSize);

    /// Class isn't copyable.
    OutStreamBuf(const OutStreamBuf&) = delete;

    /// Class is movable.
    OutStreamBuf(OutStreamBuf&& other) noexcept
        : FileOwner(std::move(other)), std::streambuf(other), outbuf(std::move(other.outbuf)) {}

    virtual ~OutStreamBuf() { OutStreamBuf::sync(); }

    /// Function returns file handle which is used for writing data.
    FileHandle get_file_handle() const { return FileOwner::get_file_handle(); }

    // functions which should be implemented for std::streambuf
protected:
    /// Function writes data from a buffer to the file. Function ensures, that
    /// after return there is space in the buffer for at least one character.
    /// Function returns Traits::eof() on failure (write error).
    virtual int overflow(int c) override;

    /// Function flushes the buffer to the underlying file 
    /// (user code should use `pubsync` function for such purpose).
    virtual int sync() override;

    // Following functions exposed to enable direct acess of the buffer.
public:
    /// Function returns the pointer to the beginning of free space in the buffer.
    char* pptr() const { return std::streambuf::pptr(); }

    /// Function returns the pointer one past the end of the free space in the buffer.
    char* epptr() const { return std::streambuf::epptr(); }

    /// Function function advances free space pointer by `count` characters.
    void  pbump(int count) { return std::streambuf::pbump(count); }

private:
    std::vector<char> outbuf;  // underlying storage for the output buffer
};


/// This class implements `std::streambuf` interface for read/write data streams.
///
/// Class owns opened file descriptor and operates on this to read data.
///
/// This class exposes few private for `std::streambuf` functions to allow
/// to generate or parse data directly in the buffer, without unnecessary copying.
/// (See description of InStreamBuf or OutStreamBuf classes for details).
///
class StreamBuf : 
    virtual StreamsInternal::FileOwner,
    virtual public std::streambuf,
    public InStreamBuf,
    public OutStreamBuf
{
public:
    /// This constant defines default size of input buffer, which typically
    /// can hold few lines of the text.
    const static size_t DefaultBufferSize;

    using FileHandle = IOSystem::FileHandle;

    /// Arguments are following: `fh` -- file descriptor opened for writing,
    /// buf_size -- the size of the output buffer.
    StreamBuf(const FileHandle& fh, size_t buf_size = DefaultBufferSize)
    : FileOwner(fh),
      InStreamBuf({}, buf_size),
      OutStreamBuf({}, buf_size)
    {}

    /// Class isn't copyable.
    StreamBuf(const StreamBuf&) = delete;

    /// Class is movable.
    StreamBuf(StreamBuf&& other) noexcept :
        FileOwner(std::move(other)),
        std::streambuf(other),
        InStreamBuf(std::move(other)),
        OutStreamBuf(std::move(other))
    {}

    /// Function returns file handle which is used for reading/writing data.
    FileHandle get_file_handle() const { return FileOwner::get_file_handle(); }

public:
    /// This functions fills input buffer (it should made at least one character be
    /// available in the buffer). Return value is traits_type::eof() in case of error,
    /// or the code of next available symbol.
    virtual int underflow() override { return InStreamBuf::underflow();  }

protected:
    /// Function writes data from a buffer to the file. Function ensures, that
    /// after return there is space in the buffer for at least one character.
    /// Function returns Traits::eof() on failure (write error).
    virtual int overflow(int c) override { return OutStreamBuf::overflow(c); }

    /// Flushes buffer to the underlying file 
    /// (user code should use `pubsync` function for such purpose).
    virtual int sync() override { return OutStreamBuf::sync(); }

public:
    /// Function returns pointer to the next available character.
    char* gptr() { return std::streambuf::gptr(); }

    /// Function returns the pointer one past the end of the buffer.
    char* egptr() { return std::streambuf::egptr(); }

    /// Function advances the next pointer in the input sequence.
    void gbump(int count) { return std::streambuf::gbump(count); }

    /// Function returns the pointer to the beginning of free space in the buffer.
    char* pptr() { return std::streambuf::pptr(); }

    /// Function returns the pointer one past the end of the free space in the buffer.
    char* epptr() { return std::streambuf::epptr(); }

    /// Function function advances free space pointer by `count` characters.
    void  pbump(int count) { return std::streambuf::pbump(count); }
};


/// This class is similar to std::ifstream, but allows to work with any file
/// descriptors (sockets, pipes, etc...) and allows to get file handle of
/// underlying file.
class InStream : private StreamsInternal::BufferOwner<InStreamBuf>, public std::istream
{
public:
    /// Underlying `InStreamBuf` buffer should be created separately and passed by rvalue reference.
    InStream(InStreamBuf&& isb) : BufferOwner{std::move(isb)}, std::istream(&buffer) {}

    /// Class is not copyable.
    InStream(const InStream&) = delete;

    /// Class is moveable.
    InStream(InStream&& other) noexcept : BufferOwner{std::move(other.buffer)}, std::istream(&buffer) {}

    /// Function returns file handle which is used for reading data.
    IOSystem::FileHandle get_file_handle() const
    {
        return dynamic_cast<InStreamBuf*>(rdbuf())->get_file_handle();
    }
};


/// This class is similar to std::ofstream, but allows to work with any file
/// descriptors (sockets, pipes, etc...) and allows to get file handle of
/// underlying file.
class OutStream : private StreamsInternal::BufferOwner<OutStreamBuf>, public std::ostream
{
public:
    /// Underlying `OutStreamBuf` buffer should be created separately and passed by rvalue reference.
    OutStream(OutStreamBuf&& osb) : BufferOwner{std::move(osb)}, std::ostream(&buffer) {}

    /// Class isn't copyable.
    OutStream(const OutStream&) = delete;

    /// Class is movable.
    OutStream(OutStream&& other) noexcept : BufferOwner{std::move(other.buffer)}, std::ostream(&buffer) {}

    /// Function returns file handle which is used for writing data.
    IOSystem::FileHandle get_file_handle() const
    {
        return dynamic_cast<OutStreamBuf*>(rdbuf())->get_file_handle();
    }
};

/// This class is similar to std::fstream, but allows to work with any file
/// descriptors (sockets, pipes, etc...) and allows to get file handle of
/// underlying file.
class IOStream : private StreamsInternal::BufferOwner<StreamBuf>, public std::iostream
{
public:
    /// Underlying `StreamBuf` buffer should be created separately and passed by rvalue reference.
    IOStream(StreamBuf&& sb) : BufferOwner{std::move(sb)}, std::iostream(&buffer) {}

    /// Class isn't copyable.
    IOStream(const IOStream&) = delete;

    /// Class is movable.
    IOStream(IOStream&& other) noexcept : BufferOwner{std::move(other.buffer)}, std::iostream(&buffer) {}

    /// Function returns file handle which is used for reading/writing data.
    IOSystem::FileHandle get_file_handle() const
    {
        return dynamic_cast<StreamBuf*>(rdbuf())->get_file_handle();
    }
};


/// This is dummy streambuf class, which can be used to count number of printed characters.
class CountingStreamBuf : public std::streambuf
{
    using traits_type = std::streambuf::traits_type;

public:
    /// Default class constructor.
    CountingStreamBuf() : count(0) { setp(buf, buf + BufSize - OverflowChars); }

    /// Class isn't copyable.
    CountingStreamBuf(const CountingStreamBuf&) = delete;

    /// Class is movable.
    CountingStreamBuf(CountingStreamBuf&& other) noexcept : std::streambuf(other), count(other.count) {}

    /// This function resets number of counted characters.
    void reset() { pubsync(), count = 0; }

    /// Function returns number of printed characters.
    uintmax_t size() { return pubsync(), count; }

    // functions which should be implemented for std::streambuf
protected:
    virtual int sync() override
    {
        count += pptr() - pbase();
        setp(pbase(), epptr());
        return 0;
        
    }

    virtual int overflow(int c) override
    {
        sync();

        if (!traits_type::eq_int_type(c, traits_type::eof()))
            ++count;

        return traits_type::not_eof(c);
    }

private:
    static const size_t BufSize = 256;
    static const size_t OverflowChars = 1;
    char buf[BufSize];
    uintmax_t count;
};

/// This is dummy output stream object which can be used to count number of printed characters.
class CountingStream : private StreamsInternal::BufferOwner<CountingStreamBuf>, public std::ostream
{
public:
    /// Default class constructor.
    CountingStream() : BufferOwner(), std::ostream(&buffer) {}

    /// Class isn't copyable.
    CountingStream(const CountingStream&) = delete;

    /// Class is movable.
    CountingStream(CountingStream&& other) noexcept : BufferOwner(std::move(other)), std::ostream(other.rdbuf()) {}

    /// Function resets numbed of counted characters.
    void reset() { flush(), buffer.reset(); }

    /// Function returns numbed of printed characters.
    uintmax_t size() { return flush(), buffer.size(); }

private:
    CountingStreamBuf buffer;
};

} // ::netcoredbg
