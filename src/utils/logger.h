// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

// This file implements interface of Tizen's logger for linux/windows.
// See Tizen's documentation and source code for details.
// This file is usable only with C++ source code (plain C not supported).
// This file might be used in place of dlog.h on Tizen platform too
// (actually Tizen's logger functions will be called).

#pragma once

#include <stddef.h>
#include <stdarg.h>

#ifdef _MSC_VER
#include <stdio.h>
#endif

#ifndef __cplusplus
#error "This file applicable only in C++ source code, plain C not supported."
#endif

// This is the local tag used for the following simplified logging macros. 
// You can change this preprocessor definition before using the other macros to change the tag.
#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

// Log levels as defined in Tizen.
typedef enum {
    DLOG_UNKNOWN = 0,
    DLOG_DEFAULT,
    DLOG_DEBUG,
    DLOG_INFO,
    DLOG_WARN,
    DLOG_ERROR,
    DLOG_FATAL
} log_priority;

// Main Tizen's log function:
// this function writes log message with given priority and tag...
extern "C" int dlog_print(log_priority prio, const char *tag, const char *fmt, ...)
#ifndef _MSC_VER
__attribute__((format(printf, 3, 4)))  // check printf arguments (GCC/Clang only)
#endif
;

// Alternative for case, when arguments passed as va_args.
extern "C" int dlog_vprint(log_priority prio, const char *tag, const char *fmt, va_list ap);

// Possible results of dlog_printf() function call.
#define DLOG_ERROR_INVALID_PARAMETER (-1)
#define DLOG_ERROR_NOT_PERMITTED (-2)


// All definitions in this namespace intendent only for internal usage.
namespace DLogInternal
{
    // This function computes file path (directory component) length at compile time.
    template <size_t N>
    constexpr size_t path_len(const char (&path)[N], size_t pos = N - 1)
    {
        return (path[pos] == '/' || path[pos] == '\\') ? pos + 1 : pos ? path_len(path, pos - 1) : 0;
    }

    // This function computes length of function name only for given function signature.
    template <size_t N>
    constexpr size_t funcname_len(const char (&sig)[N], size_t pos = 0)
    {
        return (sig[pos] >= 'A' && sig[pos] <= 'Z') || (sig[pos] >= 'a' && sig[pos] <= 'z')
            || (sig[pos] >= '0' && sig[pos] <= '9') || sig[pos] == '_' || sig[pos] == '$' || sig[pos] == ':'
            ? funcname_len(sig, pos + 1) : pos;
    }

    #ifndef _MSC_VER
    inline int __attribute__((format(printf, 1, 2))) check_args(const char *, ...) { return 0; }
    #endif

    struct LogFuncEntry
    {
        const char *const func;

        LogFuncEntry(const char *func) : func(func)
        {
            dlog_print(DLOG_DEBUG, "ENTRY", "%s", func);
        }

        ~LogFuncEntry()
        {
            dlog_print(DLOG_DEBUG, "LEAVE", "%s", func);
        }
    };
}


// Following macros shouldn't be used directly, it is intendent for internal use.
#define LOG_S__(str) #str
#define LOG_S_(str) LOG_S__(str)

// With Visual Studio's compiler arguments checking performed via (eliminated from code) call to printf.
#ifdef _MSC_VER
#define LOG_CHECK_ARGS_(fmt, ...) (false ? printf(fmt, ##__VA_ARGS__) : 0)
#else
#define LOG_CHECK_ARGS_(fmt, ...) (false ? DLogInternal::check_args(fmt, ##__VA_ARGS__) : 0)
#endif

// Following macros shouldn't be used directly, it is intendent for internal use.
#define LOG_(prio, tag, fmt, ...) \
        (LOG_CHECK_ARGS_(fmt, ##__VA_ARGS__), \
        dlog_print(prio, tag, "%.*s: %.*s(%.*s) > " fmt, \
            int(sizeof(__FILE__) - DLogInternal::path_len(__FILE__)), &__FILE__[DLogInternal::path_len(__FILE__)], \
            int(DLogInternal::funcname_len(__func__)), __func__, \
            int(sizeof(LOG_S_(__LINE))), LOG_S_(__LINE__), \
            ##__VA_ARGS__))

// These macros intendent to send a main log message using the current LOG_TAG.
// Similar macros defined in original Tizen's dlog.h.
#ifdef DEBUG 
#define LOGD(fmt, ...) LOG_(DLOG_DEBUG, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) LOG_CHECK_ARGS_(fmt, ##__VA_ARGS__)
#endif

#define LOGI(fmt, ...) LOG_(DLOG_INFO, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_(DLOG_WARN, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_(DLOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) LOG_(DLOG_FATAL, LOG_TAG, fmt, ##__VA_ARGS__)

// This macro allows to specify priority and a tag. 
// The macro definition is similar to original from Tizen's dlog.h
#define LOG(priority, tag, format, ...) LOG_(D##priority, tag, format, ##__VA_ARGS__)

// Added for compatibility with old logger, should be removed in future (DONT USE)
#ifdef DEBUG
#ifdef _WIN32
#define __CROSS_FUNCTION__ __FUNCSIG__
#elif defined(__GNUC__)
#define __CROSS_FUNCTION__ __PRETTY_FUNCTION__
#else
#define __CROSS_FUNCTION__ __func__
#endif
#define LogFuncEntry() DLogInternal::LogFuncEntry _func_entry_(__CROSS_FUNCTION__)
#else
#define LogFuncEntry() do {} while(0)
#endif

// Macros for internal usage.
#define LOG_IF_(prio, tag, expr, fmt, ...)  ((expr) ? true : \
        LOG_(prio, tag, "expression '%.*s' failed: " fmt, int(sizeof(#expr)), #expr, ##__VA_ARGS__), false)

// Macros which is absent in dlog.h, added for Pavel Orekhov.
// This macros allows to call some function and if function returns negative (false) result,
// log some error message. First argument of macros should be logical expression (function call)
// which returns boolean expression.
//
// Usage example:
//
//    HRESULT Status;
//    if (LOGD_IF(FAILED(Status = ApiCall(x, y)), "x = %d, y = %d", x, y))
//      return E_FAIL;
//
#ifdef DEBUG
#define LOGD_IF(fmt, ...) LOG_IF_(DLOG_DEFAULT, LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGD_IF(fmt, ...) LOG_CHECK_ARGS_(fmt, ##__VA_ARGS__)
#endif

#define LOGI_IF(fmt, ...) LOG_IF(DLOG_INFO, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW_IF(fmt, ...) LOG_IF(DLOG_WARNING, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE_IF(fmt, ...) LOG_IF(DLOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)

