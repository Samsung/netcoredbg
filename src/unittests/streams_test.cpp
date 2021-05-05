// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>
#include <string.h>

#include "utils/iosystem.h"
#include "utils/streams.h"

using namespace netcoredbg;

static const char test_str[] = "A quick brown fox jumps over the lazy dog.";


TEST_CASE("Streams::InStreamBuf")
{
    // create pair of pipes
    auto pipe = IOSystem::unnamed_pipe();
    REQUIRE(pipe.first);
    REQUIRE(pipe.second);

    {
        InStreamBuf buf(pipe.first, 256); // buffer size << PIPE_BUF

        IOSystem::write(pipe.second, "1", 1);
        CHECK(buf.underflow() != std::streambuf::traits_type::eof());
        CHECK(buf.sbumpc() == '1');

        // check, that EOF can be detected on reading
        IOSystem::close(pipe.second);
        CHECK(buf.underflow() == std::streambuf::traits_type::eof());
    }
    
    // check, that file was closed by FileOwner class
    CHECK(IOSystem::close(pipe.first).status != IOSystem::IOResult::Success);
}


TEST_CASE("Streams::OutStreamBuf")
{
    // create pair of pipes
    auto pipe = IOSystem::unnamed_pipe();
    REQUIRE(pipe.first);
    REQUIRE(pipe.second);

    {
        OutStreamBuf buf(pipe.second, 256); // buffer size << PIPE_BUF

        // check, that EOF can be detected on writing
        CHECK(buf.sputn(test_str, sizeof(test_str)-1) == sizeof(test_str)-1);
        CHECK(buf.pubsync() == 0);

        IOSystem::close(pipe.first);
        buf.sputn(test_str, sizeof(test_str)-1);
        CHECK(buf.pubsync() == -1);
    }
    
    // check, that file was closed by FileOwner class
    CHECK(IOSystem::close(pipe.first).status != IOSystem::IOResult::Success);
}


// This test only tests, that code can be compiled.
TEST_CASE("Streams::StreamBuf")
{
    StreamBuf buf(std::get<IOSystem::Stderr>(IOSystem::get_std_files()));
}


TEST_CASE("Streams::InStream")
{
    // TODO duplicate handle, check for reading...
    InStream stream(InStreamBuf(std::get<IOSystem::Stdin>(IOSystem::get_std_files())));
}


TEST_CASE("Streams::OutStream")
{
    // create pair of pipes
    auto pipe = IOSystem::unnamed_pipe();
    REQUIRE(pipe.first);
    REQUIRE(pipe.second);

    {
        OutStream ostream(OutStreamBuf(pipe.second));
        CHECK(ostream.good());

        ostream.write(test_str, sizeof(test_str)-1);
        CHECK(ostream.good());
    }
    return;
    char buf[1024];
    auto result = IOSystem::read(pipe.first, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size >= sizeof(test_str)-1);
    CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));
    return;
    
    CHECK(IOSystem::close(pipe.first).status == IOSystem::IOResult::Success);
    return;
    CHECK(IOSystem::close(pipe.second).status == IOSystem::IOResult::Success);
}


// This test only tests, that code can be compiled.
TEST_CASE("Streams::IOStream")
{
    IOStream stream(StreamBuf(std::get<IOSystem::Stdin>(IOSystem::get_std_files())));
}

