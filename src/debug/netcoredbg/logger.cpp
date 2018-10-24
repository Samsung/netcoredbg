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
        void vlog(LogLevel level, const std::string& fmt, va_list args) override;
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

void DlogLogger::vlog(LogLevel level, const std::string &fmt, va_list args)
{
    dlog_vprint(MapLogLevel(level), "NETCOREDBG", fmt.c_str(), args);
}


#endif



class NoLogger : public LoggerImpl
{
    public:
        NoLogger() {}
        ~NoLogger() override {}
        void log(LogLevel level, const std::string& msg) override {}
        void vlog(LogLevel level, const std::string& fmt, va_list args) override {}
};

class FileLogger : public LoggerImpl
{
    private:
        static const std::string filenameBase;
        static const std::string debugStr;
        static const std::string infoStr;
        static const std::string warnStr;
        static const std::string errorStr;

        FILE *logFile;
        std::time_t timeNow;

        static std::string FormatMessageString(LogLevel level, const std::string &str);
        static const std::string& LevelToString(LogLevel level);

    public:
        FileLogger();
        ~FileLogger() override;
        void log(LogLevel level, const std::string& msg) override;
        void vlog(LogLevel level, const std::string& fmt, va_list args) override;
};

const std::string FileLogger::filenameBase = "netcoredbg_";
const std::string FileLogger::debugStr = "DEBUG";
const std::string FileLogger::infoStr = "INFO";
const std::string FileLogger::warnStr = "WARN";
const std::string FileLogger::errorStr = "ERROR";


FileLogger::FileLogger()
{
    auto time = std::time(nullptr);
    std::ostringstream oss;

    oss << std::put_time(std::localtime(&time), "%Y_%m_%d__%H_%M_%S");

    logFile = fopen(std::string(filenameBase + oss.str() + ".log").c_str(), "w+");
}

FileLogger::~FileLogger()
{
    fclose(logFile);
}

const std::string& FileLogger::LevelToString(LogLevel level)
{
    switch (level) {
    case LOG_INFO:
        return infoStr;
    case LOG_WARN:
        return warnStr;
    case LOG_ERROR:
        return errorStr;
    case LOG_DEBUG:
    default:
        return debugStr;
    }
}

std::string FileLogger::FormatMessageString(LogLevel level, const std::string &str)
{
    auto time = std::time(nullptr);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%y-%m-%d %OH:%OM:%OS") << " " << LevelToString(level) << " " << str << std::endl;

    return oss.str();
}

void FileLogger::log(LogLevel level, const std::string& msg)
{
    if (logFile != NULL) {
        fprintf(logFile, "%s", FormatMessageString(level, msg).c_str());
        fflush(logFile);
    }
}

void FileLogger::vlog(LogLevel level, const std::string& fmt, va_list args)
{
    if (logFile != NULL) {
        vfprintf(logFile, FormatMessageString(level, fmt).c_str(), args);
        fflush(logFile);
    }
}





std::shared_ptr<LoggerImpl> Logger::logger = std::make_shared<NoLogger>();
const std::string Logger::fileStr = "file";
const std::string Logger::nologStr = "off";
#ifdef DEBUGGER_FOR_TIZEN
const std::string Logger::dlogStr = "dlog";
#endif

int Logger::setLogging(const std::string &type)
{
    if (type == Logger::fileStr)
    {
        logger = std::make_shared<FileLogger>();
    }
#ifdef DEBUGGER_FOR_TIZEN
    else if (type == Logger::dlogStr)
    {
        logger = std::make_shared<DlogLogger>();
    }
#endif
    else
    {
        logger = std::make_shared<NoLogger>();

        if (type != Logger::nologStr)
            return -1;
    }

    return 0;
}

void Logger::levelLog(LogLevel level, const std::string fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger->vlog(level, fmt, args);
    va_end(args);
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
