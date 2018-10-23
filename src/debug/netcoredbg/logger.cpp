#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <chrono>
#include <stdio.h>
#include "logger.h"

#ifdef DEBUGGER_FOR_TIZEN
#include "dlog/dlog.h"


class DlogLogger : public LoggerImpl
{
    private:
        static log_priority MapLogLevel(LogLevel level);

    public:
        DlogLogger() {}
        ~DlogLogger() override {}
        void log(LogLevel level, const std::string& msg) override;
        void vlog(LogLevel level, const std::string fmt, va_list args) override;
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

void DlogLogger::log(LogLevel level, const std::string &msg)
{
    dlog_print(MapLogLevel(level), "NETCOREDBG", "%s", msg.c_str());
}

void DlogLogger::vlog(LogLevel level, const std::string fmt, va_list args)
{
    dlog_vprint(MapLogLevel(level), "NETCOREDBG", fmt.c_str(), args);
}


#endif



class NoLogger : public LoggerImpl
{
    public:
        NoLogger() {}
        ~NoLogger() override {}
        void log(LogLevel level, const std::string &msg) override {}
        void vlog(LogLevel level, const std::string fmt, va_list args) override {}
};

class FileLogger : public LoggerImpl
{
    LogLevel l;

    private:
        const std::string filenameBase = "netcoredbg_";
        FILE *logFile;
        std::time_t timeNow;

        static std::string FormatMessageString(const std::string &str);

    public:
        FileLogger(LogLevel level);
        ~FileLogger() override;
        void log(LogLevel level, const std::string& msg) override;
        void vlog(LogLevel level, const std::string fmt, va_list args) override;
};



FileLogger::FileLogger(LogLevel level)
{
    auto time = std::time(nullptr);
    std::ostringstream oss;

    oss << std::put_time(std::localtime(&time), "%Y_%m_%d__%H_%M_%S");

    logFile = fopen(std::string(filenameBase + oss.str() + ".log").c_str(), "w+");
    l = level;
}

FileLogger::~FileLogger()
{
    fclose(logFile);
}

std::string FileLogger::FormatMessageString(const std::string &str)
{
    auto time = std::time(nullptr);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%y-%m-%d %OH:%OM:%OS") << " " << str << std::endl;

    return oss.str();
}

void FileLogger::log(LogLevel level, const std::string& msg)
{
    if (level < l)
        return;

    if (logFile != NULL) {
        fprintf(logFile, "%s", FormatMessageString(msg).c_str());
        fflush(logFile);
    }
}

void FileLogger::vlog(LogLevel level, const std::string fmt, va_list args)
{
    if (level < l)
        return;

    if (logFile != NULL) {
        vfprintf(logFile, FormatMessageString(fmt).c_str(), args);
        fflush(logFile);
    }
}





std::shared_ptr<LoggerImpl> Logger::logger = std::make_shared<NoLogger>();

void Logger::setLogging(LogType type, LogLevel level)
{
    switch (type) {
    case FILE_LOG:
        logger = std::make_shared<FileLogger>(level);
        break;
    case DLOG_LOG:
#ifdef DEBUGGER_FOR_TIZEN
        logger = std::make_shared<DlogLogger>();
        break;
#endif
    case NO_LOG:
    default:
        logger = std::make_shared<NoLogger>();
        break;
    }
}

void Logger::log(const std::string fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger->vlog(LOG_INFO, fmt, args);
    va_end(args);
}

FuncLogger Logger::getFuncLogger(const std::string &func)
{
    return FuncLogger(logger, func);
}
