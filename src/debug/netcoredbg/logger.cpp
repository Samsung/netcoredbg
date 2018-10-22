#include <string>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <chrono>
#include "logger.h"




class NoLogger : public LoggerImpl
{
    public:
        NoLogger() {}
        ~NoLogger() override {}
        void log(LogLevel level, const std::string& msg) override {}
};

class FileLogger : public LoggerImpl
{
    LogLevel l;

    private:
        const std::string filenameBase = "netcoredbg_";
        std::ofstream logFile;
        std::time_t timeNow;

    public:
        FileLogger(LogLevel level);
        ~FileLogger() override;
        void log(LogLevel level, const std::string& msg) override;

};



FileLogger::FileLogger(LogLevel level)
{
    auto time = std::time(nullptr);
    std::ostringstream oss;

    oss << std::put_time(std::localtime(&time), "%Y_%m_%d__%H_%M_%S");

    logFile.open(filenameBase + oss.str() + ".log", std::ios::out | std::ios::trunc);
    l = level;
}

FileLogger::~FileLogger()
{
    logFile.close();
}

void FileLogger::log(LogLevel level, const std::string& msg)
{
    if (level < l)
        return;

    auto time = std::time(nullptr);

    if (logFile.is_open()) {
        logFile << std::put_time(std::localtime(&time), "%y-%m-%d %OH:%OM:%OS") << " " << msg + "\n";
        logFile.flush();
    }
}





std::shared_ptr<LoggerImpl> Logger::logger = std::make_shared<NoLogger>();

void Logger::setLogging(LogType type, LogLevel level)
{
    switch (type) {
    case FILE_LOG:
        logger = std::make_shared<FileLogger>(level);
        break;
    case NO_LOG:
    default:
        logger = std::make_shared<NoLogger>();
        break;
    }
}

void Logger::log(const std::string& msg)
{
    logger->log(LOG_INFO, msg);
}

FuncLogger Logger::getFuncLogger(const std::string &func)
{
    return FuncLogger(logger, func);
}
