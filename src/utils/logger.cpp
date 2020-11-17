// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

// This file implements interface of Tizen's logger for linux/windows.
// See Tizen's documentation and source code for details.
//
// Note: this file should be excluded from build on Tizen -- in this
// case Tizen's logger function should be linked.

#ifdef DEBUGGER_FOR_TIZEN
#error "This file should be excluded from build on Tizen"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#define LINE_MAX 2048
#endif

#ifdef __unix__
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

#include "logger.h"

namespace
{
    char log_buffer[2*LINE_MAX];

    // Implementation clock_gettime(CLOCK_MONOTONIC, ...) for Windows.
    #ifdef _WIN32
    enum { CLOCK_MONOTONIC = 0 };

    int clock_gettime(int tsrc, struct timespec* ts)
    {
        (void)tsrc;
        assert(tsrc == CLOCK_MONOTONIC);

        static __int64 base = []() {
                __int64 t;  GetSystemTimeAsFileTime((FILETIME*)&t); return t;
        } ();

        __int64 cur;
        GetSystemTimeAsFileTime((FILETIME*)&cur);
        cur -= base;
        ts->tv_sec = time_t(cur / 10000000i64), ts->tv_nsec = long(cur % 10000000i64 * 100);
        return 0;
    }
    #endif

    // Function returns thread identifier.
    unsigned get_tid()
    {
        #ifndef _WIN32
        static thread_local unsigned thread_id = syscall(SYS_gettid);
        #else
        static thread_local unsigned thread_id = unsigned(GetCurrentThreadId());
        #endif

        return thread_id;
    }

    // Function returns process identifier.
    int get_pid()
    {
        #ifndef _WIN32
        static unsigned process_id = ::getpid();
        #else
        static unsigned process_id = unsigned(GetCurrentProcessId());
        #endif

        return process_id;
    }

    // This function opens log file, log file name is determined
    // by contents of environment variable "LOG_OUTPUT".
    FILE* open_log_file()
    {
        const char *env = getenv("LOG_OUTPUT");
        if (!env)
            return nullptr;   // log disabled

        if (!strcmp("stdout", env))
            return stdout;

        if (!strcmp("stderr", env))
            return stderr;

        FILE *result = fopen(env, "a");
        if (!result)
        {
            perror(env);
            return nullptr;
        }

        setvbuf(result, log_buffer, _IOFBF, sizeof(log_buffer));
        return result;
    }
}


extern "C" int dlog_print(log_priority prio, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    dlog_vprint(prio, tag, fmt, args);
    va_end(args);
    return 0;
}

// Function should form output line like this:
//
// 1500636976.777 I/HARDWARE(P 2293, T 2293): udev.c: uevent_control_cb(62) > Set udev monitor buffer size 131072
// ^             ^  ^          ^       ^      ^       ^                 ^     ^
// |             |  ` tag      `pid    ` tid  |       ` function name   |     ` user provided message...
// |             ` log level                  ` file name               ` line number
// `--- time sec.msec
//
extern "C" int dlog_vprint(log_priority prio, const char *tag, const char *fmt, va_list ap)
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    if (prio == DLOG_DEFAULT)
        prio = DLOG_INFO;

    char level = 'I';
    if (prio >= DLOG_DEBUG && prio <= DLOG_FATAL)
        level = "DIWEF"[prio - DLOG_DEBUG];

    static FILE *log_file = open_log_file();
    if (log_file == NULL || ferror(log_file))
        return DLOG_ERROR_NOT_PERMITTED;

    int len = fprintf(log_file, "%lu.%03u %c/%s(P%4u, T%4u): ",
                long(ts.tv_sec & 0x7fffff), int(ts.tv_nsec / 1000000), level, tag, get_pid(), get_tid());

    int r = vfprintf(log_file, fmt, ap);
    if (r < 0) {
        fputc('\n', log_file);
        return DLOG_ERROR_INVALID_PARAMETER;
    }

    fputc('\n', log_file);
    return fflush(log_file) < 0 ? DLOG_ERROR_NOT_PERMITTED : len + r + 1;
}

