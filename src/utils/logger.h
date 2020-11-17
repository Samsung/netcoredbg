#pragma once

#include <stdio.h>
#include <memory>
#include <cstdarg>

enum LogType {
    NO_LOG = 0,
    FILE_LOG,
    DLOG_LOG,
};

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};


class LoggerImpl
{
    public:
        virtual ~LoggerImpl() {};
        virtual void vlog(LogLevel level, const char *fmt, va_list args) = 0;
        virtual void log(LogLevel level, const char *msg) = 0;
};

class FuncLogger
{
    private:
        const std::shared_ptr<LoggerImpl> &logger;
        unsigned len;
        const char *func;

        void log(LoggerImpl* logger, LogLevel level, const char *fmt, ...);

    public:
        FuncLogger(const std::shared_ptr<LoggerImpl> &logger, unsigned len, const char *func)
        : logger(logger), len(len), func(func)
        {
            log(logger.get(), LOG_DEBUG, "> %.*s", len, func);
        }

        ~FuncLogger()
        {
           log(logger.get(), LOG_DEBUG, "< %.*s", len, func);
        }
};

class Logger
{
    private:
        static std::shared_ptr<LoggerImpl> logger;

    public:
        Logger() {}
        static int setLogging(const char *type);
        static void levelLog(LogLevel level, const char *fmt, ...);
        static void log(const char *fmt, ...);

        static FuncLogger getFuncLogger(unsigned len, const char *func)
        {
            return FuncLogger(logger, len, func);
        }
};

#ifdef WIN32
#define __CROSS_FUNCTION__ __FUNCSIG__
#ifndef DIRECTORY_SEPARATOR_STR_A
#define DIRECTORY_SEPARATOR_STR_A "\\"
#endif
#else // WIN32
#define __CROSS_FUNCTION__ __PRETTY_FUNCTION__
#ifndef DIRECTORY_SEPARATOR_STR_A
#define DIRECTORY_SEPARATOR_STR_A "/"
#endif
#endif // WIN32

#define LogFuncEntry()  \
    FuncLogger __funcLogger__ = Logger::getFuncLogger(sizeof(__CROSS_FUNCTION__)-1, __CROSS_FUNCTION__);


#define __FILENAME__ (strrchr(DIRECTORY_SEPARATOR_STR_A __FILE__, DIRECTORY_SEPARATOR_STR_A[0]) + 1)


namespace LoggerInternal
{
    template <size_t N>
    constexpr size_t path_len(const char (&s)[N], size_t pos = N-1)
    {
        return (s[pos] == '/' || s[pos] == '\\') ? pos + 1 : pos ? path_len(s, pos - 1) : 0;
    }
}

#define LogWithLine(fmt, ...) (false ? (void)printf((fmt), ##__VA_ARGS__) : \
    Logger::log(("[%s:%u] " fmt),&__FILE__[LoggerInternal::path_len(__FILE__)], __LINE__, ##__VA_ARGS__))

#define LogLevelWithLine(level, fmt, ...) (false ? (void)printf((fmt), ##__VA_ARGS__) : \
    Logger::levelLog(level, ("[%s:%u] " fmt),&__FILE__[LoggerInternal::path_len(__FILE__)], __LINE__, ##__VA_ARGS__))

