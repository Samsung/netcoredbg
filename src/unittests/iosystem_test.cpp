// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// Licensed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <catch2/catch.hpp>

#define COMPILE_TEST_ASSERTS true
#include "compile_test.h"

#include <sys/types.h>
#include <string.h>
#include <thread>
#include <chrono>

#include "utils/iosystem.h"

#ifndef WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int Socket;
const Socket INVALID_SOCKET = -1;
typedef int HANDLE;

#else // WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET Socket;

void usleep(unsigned long usec) { Sleep((usec+999)/1000); }

static int system(const char *command)
{
    char shell[MAX_PATH];
    size_t size = GetEnvironmentVariableA("COMSPEC", (LPSTR)&shell, sizeof(shell));
    if (size == 0 || size >= sizeof(shell))
        return -1;

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    auto args = std::string("/c ") + command;
    if (!CreateProcessA(shell, (LPSTR)args.c_str(),
                        NULL, NULL,
                        TRUE /* inherit handles */,
                        CREATE_NO_WINDOW,
                        NULL, NULL,
                        &si, &pi))
    {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

#endif // WIN32


using namespace netcoredbg;

const char test_str[] = "A quick brown fox jumps over the lazy dog.";


TEST_CASE("IOSystem::handle")
{
    REQUIRE(!!IOSystem::FileHandle() == false);

    auto std_files = IOSystem::get_std_files();
    REQUIRE(!!std::get<IOSystem::Stdin>(std_files) == true);
    REQUIRE(!!std::get<IOSystem::Stdout>(std_files) == true);
    REQUIRE(!!std::get<IOSystem::Stderr>(std_files) == true);
}


TEST_CASE("IOSystem::pipe")
{
    // create pair of pipes
    auto pipe = IOSystem::unnamed_pipe();
    REQUIRE(pipe.first);
    REQUIRE(pipe.second);

    // write test string to writing end of the pipe
    auto result = IOSystem::write(pipe.second, test_str, sizeof(test_str)-1);
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);

    // read stirng back from reading end of the pipe and check it
    char buf[1024];
    result = IOSystem::read(pipe.first, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);
    CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));

    // check, that you will get EOF on reading end of the pipe when closing writing end
    IOSystem::close(pipe.second);
    result = IOSystem::read(pipe.first, buf, sizeof(buf));
#ifndef WIN32
    CHECK(result.status == IOSystem::IOResult::Eof);
#else
    CHECK(result.status == IOSystem::IOResult::Error); // TODO
#endif
    IOSystem::close(pipe.first);
}


// Function creates TCP socket and connects to specified port on localhost.
// Return value is empty FileHandle (in case of error) or FileHandle containing connected socket.
IOSystem::FileHandle connect_to(unsigned port)
{
    Socket s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return {};

    struct sockaddr_in addr; 
    addr.sin_family = AF_INET; 
    addr.sin_port = htons(port); 
       
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) < 0)
        return {};

    if (::connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
        return {};
    
    return {s};
}


// Function creates pair of connected TCP sockets.
// Return value is pair of FileHandles contaning connected sockets (server and client, respectively),
// or pair of empty FileHandles (in case of error).
std::pair<IOSystem::FileHandle, IOSystem::FileHandle> socket_pair(void)
{
    unsigned port;
    IOSystem::FileHandle conn, sock;

    srand(unsigned(time(NULL)));
    for (unsigned retry = 0; retry < 10; retry++)
    {
        // selecting random port in range 1024..32767
        port = rand()%32768 + 1024;
        volatile bool fin = false;

        std::thread thread {
            [&]{
                while (!fin && !conn)
                {
                    usleep(100000);
                    conn = connect_to(port);
                }
                return conn;
            }
        };

        sock = IOSystem::listen_socket(port);
        fin = true;
        thread.join();

        if (sock) break;

        if (conn) IOSystem::close(conn);
    }

    REQUIRE(conn);
    REQUIRE(sock);

    return {sock, conn};
}


TEST_CASE("IOSystem::socket")
{
    char buf[1024];

    // create pair of connected sockets
    IOSystem::FileHandle conn, sock;
    std::tie(sock, conn) = socket_pair();

    // write test string to "client" socket
    auto result = IOSystem::write(conn, test_str, sizeof(test_str)-1);
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);

    // check that test string is readable from "server" socket
    result = IOSystem::read(sock, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);
    CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));

    // write test string to "server" socket
    result = IOSystem::write(sock, test_str, sizeof(test_str)-1);
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);

    // check that test string is readable from "client" socket
    result = IOSystem::read(conn, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size == sizeof(test_str)-1);
    CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));

    // close "client" socket and check for EOF condition on "server" socket
    IOSystem::close(conn);
    result = IOSystem::read(sock, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Eof);
    IOSystem::close(sock);
}


TEST_CASE("IOSystem::StdIOSwap")
{
    char buf[1024];

    // create pipes for stdout, stderr, stdin.
    const unsigned NP = std::tuple_size<IOSystem::StdFiles>::value;
    std::pair<IOSystem::FileHandle, IOSystem::FileHandle> pipes[NP];
    for (unsigned n = 0; n < NP; n++)
    {
        pipes[n] = IOSystem::unnamed_pipe();
        REQUIRE(pipes[n].first);
        REQUIRE(pipes[n].second);
    }

    // write test string to stdin pipe
    auto result = IOSystem::write(pipes[0].second, test_str, sizeof(test_str)-1);
    REQUIRE(result.status == IOSystem::IOResult::Success);
    REQUIRE(result.size == sizeof(test_str)-1);

    {
        IOSystem::StdIOSwap fds({pipes[0].first, pipes[1].second, pipes[2].second});

        // check stdin pipe
        result = IOSystem::read(std::get<IOSystem::Stdin>(IOSystem::get_std_files()), buf, sizeof(buf));
        CHECK(result.status == IOSystem::IOResult::Success);
        CHECK(result.size == sizeof(test_str)-1);
        CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));

        // write to stdout/stderr, also check that stdout is inherited by child process
        REQUIRE(system("echo STDOUTstdout") == 0);

        result = IOSystem::write(std::get<IOSystem::Stderr>(IOSystem::get_std_files()), test_str, sizeof(test_str)-1);
        REQUIRE(result.status == IOSystem::IOResult::Success);
        REQUIRE(result.size == sizeof(test_str)-1);
    }

    // check that test string is readable from stdout pipe
    result = IOSystem::read(pipes[1].first, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    static const char stdout_test[] = "STDOUTstdout";
    CHECK(result.size >= sizeof(stdout_test));  // +1 byte for \n
    CHECK(!strncmp(stdout_test, buf, sizeof(stdout_test)-1));

    // check test string availablility for stderr pipe
    result = IOSystem::read(pipes[2].first, buf, sizeof(buf));
    CHECK(result.status == IOSystem::IOResult::Success);
    CHECK(result.size >= sizeof(test_str)-1);
    CHECK(!strncmp(test_str, buf, sizeof(test_str)-1));

    // close all pipes
    for (unsigned n = 0; n < NP; n++)
    {
        CHECK(IOSystem::close(pipes[n].second).status == IOSystem::IOResult::Success);
        CHECK(IOSystem::close(pipes[n].first).status == IOSystem::IOResult::Success);
    }
}


// Function checks select function with provided pair of pipes or sockets.
// This function closes pipes or sockets given as argument.
// NOTE: this function only tests file descriptors for reading!
// TODO: add test for write and exceptios.
void check_async(std::pair<IOSystem::FileHandle, IOSystem::FileHandle> pipe)
{
    // check select with no events
    char buf[1024];
    IOSystem::AsyncHandle h = IOSystem::async_read(pipe.first, buf, sizeof(buf));
    REQUIRE(!!h);
    CHECK(! IOSystem::async_wait(&h, &h + 1, std::chrono::milliseconds(100)));
    auto result = IOSystem::async_result(h);
    CHECK(result.status == IOSystem::IOResult::Pending);
    result = IOSystem::async_cancel(h);
    CHECK(result.status == IOSystem::IOResult::Success);

    // check, that select wakes thread when data is available
    std::thread thread {
        [&]{ IOSystem::write(pipe.second, test_str, sizeof(test_str)-1); }
    };
    h = IOSystem::async_read(pipe.first, buf, sizeof(buf));
    CHECK(h);
    CHECK(IOSystem::async_wait(&h, &h + 1, std::chrono::milliseconds(100)));
    result = IOSystem::async_result(h);
    CHECK(result.status == IOSystem::IOResult::Success);
    thread.join();

    // check, that next select will be finished with timeout
    h = IOSystem::async_read(pipe.first, buf, sizeof(buf));
    CHECK(h);
    CHECK(!IOSystem::async_wait(&h, &h + 1, std::chrono::milliseconds(300)));
    result = IOSystem::async_result(h);
    CHECK(result.status == IOSystem::IOResult::Pending);
    CHECK(IOSystem::async_cancel(h).status == IOSystem::IOResult::Success);

    // check, that closing writing end of the pipe will wake thread waiting on reading end of the pipe
    std::thread thread2 {
        [&]{ IOSystem::close(pipe.second); }
    };

    h = IOSystem::async_read(pipe.first, buf, sizeof(buf));
    CHECK(h);
    CHECK(!IOSystem::async_wait(&h, &h + 1, std::chrono::milliseconds(300)));
    result = IOSystem::async_result(h);
    CHECK(result.status == IOSystem::IOResult::Eof);
    thread2.join();

    // close reading end of the pipe
    IOSystem::close(pipe.first);
}


TEST_CASE("IOSystem::select_pipe")
{
    //check_select(IOSystem::unnamed_pipe());
}

TEST_CASE("IOSystem::select_socket")
{
    //check_select(socket_pair());
}

