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
        virtual void vlog(LogLevel level, const std::string& fmt, va_list args) = 0;
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
        static const std::string fileStr;
        static const std::string nologStr;
#ifdef DEBUGGER_FOR_TIZEN
        static const std::string dlogStr;
#endif

        static std::shared_ptr<LoggerImpl> logger;

    public:
        Logger() {}
        static int setLogging(const std::string &type);
        static void levelLog(LogLevel level, const std::string fmt, ...);
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


#define __FILENAME__ (strrchr(__FILE__, DIRECTORY_SEPARATOR_STR_A[0]) ? strrchr(__FILE__, DIRECTORY_SEPARATOR_STR_A[0]) + 1 : __FILE__)

#define LogWithLine(fmt, ...) \
    Logger::log("[" + std::string(__FILENAME__) + ":" + std::to_string(__LINE__) + "] " + fmt, ##__VA_ARGS__);

#define LogLevelWithLine(level, fmt, ...)           \
    Logger::levelLog(level, "[" + std::string(__FILENAME__) + ":" + std::to_string(__LINE__) + "] " + fmt, ##__VA_ARGS__);
