// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file ioredirect.h
/// This file contains declaration of class `IORedirectHelper`, which allows
/// to redirect standard input/output files of the program.

#pragma once
#include <cstddef>
#include <vector>
#include <thread>
#include <functional>
#include <memory>
#include <atomic>

#include "interfaces/idebugger.h"  // AsyncResult

#include "iosystem.h"
#include "streams.h"
#include "platform.h"
#include "span.h"
#include "utils/rwlock.h"

namespace netcoredbg
{

/// This class allows to redirect standart input/output file of the program
/// (and its child processes), and provides event driven mechanism for 
/// processing data which was written to stdout/stderr.
class IORedirectHelper
{
public:
    template <typename T> using span = Utility::span<T>;

    using StreamType = IOSystem::StdFileType;

    using PipePair = std::pair<IOSystem::FileHandle, IOSystem::FileHandle>; 
    using Pipes = std::tuple<PipePair, PipePair, PipePair>;

    /// Data type which represents callback functor, which is called, when
    /// some data is written to pipes representing stdout and stderr files.
    /// Arguments of the callback functor are following:
    ///  * `StreamType` defines the stream: IOSystem::Stdout or IOSystem::Stderr;
    ///  * `span<char>` represents portion of the data (text).
    using InputCallback =  std::function<void(StreamType, span<char>)>;

    /// This constant represents default buffers size for input/output.
    /// Typically buffer with default size can hold few lines of text.
    static const size_t DefaultBufferSize;

    /// Class constructor requires following arguments:
    ///  * three pair of pipes which represent stdin/stdout/sterr files;
    ///  * callback functor, which is called when some data became available in stdout/stderr;
    ///  * optionally: input (for stdout/stderr) and output (for writing to stdin) buffers sizes.
    IORedirectHelper(const Pipes&, const InputCallback&,
        size_t input_bufsize = DefaultBufferSize, size_t output_bufsize = DefaultBufferSize);

    ~IORedirectHelper();

    /// This function allows to write some data to pipe, which represents stdin stream.
    /// Output IS NOT BLOCKING, function returns actual number of written bytes
    /// (this number might be less than requested if output buffer is full).
    using AsyncResult = IDebugger::AsyncResult;
    AsyncResult async_input(InStream &stream);

    /// This function interrupts thread which is currently executing `async_input`
    /// or thread which will call `async_input` next time.
    void async_cancel();

    /// This function allows to execute some another function `func` with substituted
    /// standard input/output files. Typically function `func` should start some external
    /// process, which inherits stdin/stdout/stderr files which is substituted during
    /// function invocation. Arguments `args...` just forwared to function `func`.
    ///
    /// Note: this function closes files, so it can be called only once!
    ///
    template <typename Func, typename... Args>
    typename std::result_of<Func(Args...)>::type exec(Func func, Args&&... args)
    {
        IOSystem::StdIOSwap file_descriptors({
            std::get<IOSystem::Stdin>(m_pipes),
            std::get<IOSystem::Stdout>(m_pipes),
            std::get<IOSystem::Stderr>(m_pipes)});

        // close "remote" pipe ends
        auto on_exit = [&](void *) {
            IOSystem::close(std::get<IOSystem::Stdin>(m_pipes));
            IOSystem::close(std::get<IOSystem::Stdout>(m_pipes));
            IOSystem::close(std::get<IOSystem::Stderr>(m_pipes));
        };

        std::unique_ptr<void, decltype(on_exit)> defer{this, on_exit};
        return func(std::forward<Args>(args)...);
    }

private:
    void wake_worker();
    void wake_reader();

    void worker();    // worker thread function

    // remote side of the pipes
    const std::tuple<IOSystem::FileHandle, IOSystem::FileHandle, IOSystem::FileHandle> m_pipes;

    // our side of the pipes
    std::tuple<OutStream, InStream, InStream> m_streams;

    InputCallback m_callback;   // callback function (which in called on data receiving)

    // pointers in our side stdin's the buffer (actually output)
    // used to organize asynchronous IO
    char *m_sent;       // start of region for which async. write request issued
    char *m_unsent;     // end of such region, start region of unwritten data.

    bool m_eof;  // EOF reached in async_input, worker should close writing end of pipe

    // Synchronize access of async_input function and worker thread to 
    // stdin's output buffer and two pointers listed above (m_sent and m_unsent).
    Utility::RWLock m_rwlock;

    PipePair m_worker_pipe; // pipe to wake worker thread
    PipePair m_input_pipe;  // pipe to wake thread sleeping in async_input
    
    std::atomic<bool> m_cancel;  // atomic flag which prevents multiple calls to async_cancel()
    volatile bool     m_finish;  // exit request for worker thread

    std::thread   m_thread;     // worker threead (which monitors received data)
};

}  // ::netcoredbg
