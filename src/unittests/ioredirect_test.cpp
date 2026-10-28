// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <tuple>

#include "utils/span.h"
#include "utils/iosystem.h"
#include "utils/streams.h"
#include "utils/ioredirect.h"

#ifdef _WIN32
void usleep(unsigned long usec) { Sleep((usec+999)/1000); }
#else
#include <unistd.h>
#endif

using namespace netcoredbg;
template <typename T> using span = Utility::span<T>;

// TODO...
TEST_CASE("IORedirect::create")
{
    auto callback = [&](IORedirectHelper::StreamType stream, span<char> text)
    {
       (void)stream, (void)text; 
    };

    IORedirectHelper ior(
        { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
        callback );
}

// A process that writes its last line and exits: the reader can see that line and the end of both stdout and
// stderr in one pass, and the line must still reach the callback. The end of stderr ended the reader before the
// data already read from stdout was passed on, so the debuggee's last output was lost now and then on a busy
// machine. The callback holds the reader on the first line while the last one is written and the pipes close.
TEST_CASE("IORedirect::last output before the end of both streams")
{
    auto pipes = std::make_tuple(IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe());
    const IOSystem::FileHandle out = std::get<IOSystem::Stdout>(pipes).second;
    const IOSystem::FileHandle err = std::get<IOSystem::Stderr>(pipes).second;

    std::mutex mutex;
    std::condition_variable changed;
    bool held = false, release = false;
    std::string received;
    auto callback = [&](IORedirectHelper::StreamType stream, span<char> text)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (stream == IOSystem::Stdout)
            received.append(text.begin(), text.end());
        if (!held)
        {
            held = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }
    };

    IORedirectHelper ior(pipes, callback);

    IOSystem::write(out, "first\n", 6);
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return held; }));
    }

    // the process writes its last line and exits while the reader is busy
    IOSystem::write(out, "last\n", 5);
    IOSystem::close(out);
    IOSystem::close(err);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        changed.notify_all();
    }

    bool complete = false;
    for (int wait = 0; wait < 200 && !complete; wait++)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            complete = received == "first\nlast\n";
        }
        if (!complete)
            usleep(10000);
    }

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(received == "first\nlast\n");
}

#if 0  // IORedirect::output function should be changed on async_input().
TEST_CASE("IORedirect::basic")
{
    static const char stdout_str[] = "OUTPUT OUTPUT OUTPUT\r\n";
    static const char stderr_str[] = "ERROR ERROR ERROR\r\n";
    static char const stdin_str[] = "INPUT INPUT INPUT\r\n";

    std::string stdout_res;
    std::string stderr_res;

    auto callback = [&](IORedirectHelper::StreamType stream, span<char> text)
    {
        REQUIRE((stream == IOSystem::Stdout || stream == IOSystem::Stderr));
        (stream == IOSystem::Stdout ? stdout_res : stderr_res).append(text.begin(), text.end());
    };

    auto test = [&]()
    {
        fputs(stdout_str, stdout), fflush(stdout);
        fputs(stderr_str, stderr), fflush(stderr);
        
        char buf[1024];
        memset(buf, 0, sizeof(buf));
        fgets(buf, sizeof(buf), stdin);
        CHECK(strlen(buf) == sizeof(stdin_str) - 1);
        CHECK(!strcmp(buf, stdin_str));
    };

    {
        IORedirectHelper ior(
            { IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe(), IOSystem::unnamed_pipe() },
            callback );

        ior.output(stdin_str, sizeof(stdin_str) - 1);
        ior.exec(test);

        usleep(300000);
    }

    CHECK(stdout_str == stdout_res);
    CHECK(stderr_str == stderr_res);
}
#endif
