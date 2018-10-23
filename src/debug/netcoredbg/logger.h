#pragma once

#include <string>
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
        virtual void vlog(LogLevel level, const std::string fmt, va_list args) = 0;
        virtual void log(LogLevel level, const std::string& msg) = 0;
};

class FuncLogger
{
    private:
        const std::shared_ptr<LoggerImpl> &logger;
        const std::string func;

    public:
        FuncLogger(const std::shared_ptr<LoggerImpl> &logger, const std::string &func) : logger(logger), func(func)
        {
            logger->log(LOG_DEBUG, "> " + func);
        }

        ~FuncLogger()
        {
            logger->log(LOG_DEBUG, "< " + func);
        }
};

class Logger
{
    private:
        static std::shared_ptr<LoggerImpl> logger;

    public:
        Logger() {}
        static void setLogging(LogType type, LogLevel level = LOG_INFO);
        static void log(const std::string fmt, ...);
        static FuncLogger getFuncLogger(const std::string &func);
};

#ifdef WIN32
#define __CROSS_FUNCTION__ __FUNCSIG__
#else // WIN32
#define __CROSS_FUNCTION__ __PRETTY_FUNCTION__
#endif // WIN32

#define LogFuncEntry()  \
    FuncLogger __funcLogger__ = Logger::getFuncLogger(std::string(__CROSS_FUNCTION__));
