#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <chrono>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "utils/logger.h"

namespace 
{
    template <typename Logger>
    void log_via_va_args(Logger& logger, LogLevel level, const char *msg, ...)
    {
        va_list args;
        va_start(args, msg);
        logger.vlog(level, "%s", args);
        va_end(args);
    }
}


#ifdef DEBUGGER_FOR_TIZEN
#include "dlog/dlog.h"

class DlogLogger : public LoggerImpl
{
    private:
        static log_priority MapLogLevel(LogLevel level);

    public:
        DlogLogger() {}
        ~DlogLogger() override {}
        void log(LogLevel level, const char *msg) override;
        void vlog(LogLevel level, const char *fmt, va_list args) override;
};

log_priority DlogLogger::MapLogLevel(LogLevel level)
{
    if (level == LOG_INFO)
        return DLOG_INFO;
    else if (level == LOG_WARN)
        return DLOG_WARN;
    else if (level == LOG_ERROR)
        return DLOG_ERROR;

    return DLOG_DEBUG;
}

void DlogLogger::log(LogLevel level, const char *msg)
{
    log_via_va_args(*this, level, "%s", msg);
}

void DlogLogger::vlog(LogLevel level, const char *fmt, va_list args)
{
    dlog_vprint(MapLogLevel(level), "NETCOREDBG", fmt, args);
}

#endif // Tizen specific


const char *const levelNames[] =
{
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};


class NoLogger : public LoggerImpl
{
    public:
        NoLogger() {}
        ~NoLogger() override {}
        void log(LogLevel level, const char *msg) override {}
        void vlog(LogLevel level, const char *fmt, va_list args) override {}
};

class FileLogger : public LoggerImpl
{
    private:
        static const std::string filenameBase;
        FILE *logFile;

    public:
        FileLogger();
        ~FileLogger() override;
        void log(LogLevel level, const char *msg) override;
        void vlog(LogLevel level, const char *fmt, va_list args) override;
};


const std::string FileLogger::filenameBase = "netcoredbg_";


static void get_local_time(std::tm *tm_snapshot)
{
    auto system_now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(system_now);
#if (defined(WIN32) || defined(_WIN32) || defined(__WIN32__))
    ::localtime_s(tm_snapshot, &time);
#else
    ::localtime_r(&time, tm_snapshot); // POSIX
#endif
}

FileLogger::FileLogger()
{
    std::string tmpPath = GetTempFolder();
    std::ostringstream oss;

    std::tm tm_snapshot;
    get_local_time(&tm_snapshot);
    oss << std::put_time(&tm_snapshot, "%Y_%m_%d__%H_%M_%S");

    logFile = fopen(std::string(tmpPath + filenameBase + oss.str() + ".log").c_str(), "w+");
}

FileLogger::~FileLogger()
{
    fclose(logFile);
}


void FileLogger::log(LogLevel level, const char *msg)
{
    log_via_va_args(*this, level, "%s", msg);
}

void FileLogger::vlog(LogLevel level, const char *fmt, va_list args)
{
    if (logFile == NULL) return;

    const char *level_name = (unsigned(level) < sizeof(levelNames)/sizeof(levelNames[0]))
                            ? levelNames[level] : levelNames[LOG_DEBUG];
    std::tm tm;
    get_local_time(&tm);

    fprintf(logFile, "%04u-%02u-%02u %02u:%02u:%02u %s ",
        tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, level_name);

    vfprintf(logFile, fmt, args);
    fputc('\n', logFile);

    fflush(logFile);
}


std::shared_ptr<LoggerImpl> Logger::logger = std::make_shared<NoLogger>();


int Logger::setLogging(const char *type)
{
    if (!strcmp(type, "file"))
    {
        logger = std::make_shared<FileLogger>();
    }
#ifdef DEBUGGER_FOR_TIZEN
    else if (!strcmp(type, "dlog"))
    {
        logger = std::make_shared<DlogLogger>();
    }
#endif
    else
    {
        logger = std::make_shared<NoLogger>();

        if (strcmp(type, "off") != 0)
            return -1;
    }

    return 0;
}

void Logger::levelLog(LogLevel level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger->vlog(level, fmt, args);
    va_end(args);
}

void Logger::log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger->vlog(LOG_INFO, fmt, args);
    va_end(args);
}


void FuncLogger::log(LoggerImpl *logger, LogLevel level, const char *fmt, ...)
{
    if (logger == nullptr) return;

    va_list args;
    va_start(args, fmt);
    logger->vlog(level, fmt, args);
    va_end(args);
}
