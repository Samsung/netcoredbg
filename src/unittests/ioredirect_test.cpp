// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include <stdio.h>
#include <string.h>
#include <string>

#include "span.h"
#include "iosystem.h"
#include "streams.h"
#include "ioredirect.h"

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

