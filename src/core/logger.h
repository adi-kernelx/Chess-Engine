#pragma once

#include <string>

namespace chess {
namespace core {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static void init(LogLevel level = LogLevel::INFO);
    static void log(LogLevel level, const std::string& thread_name, const std::string& context, const std::string& message);
    
    static void debug(const std::string& thread_name, const std::string& context, const std::string& message);
    static void info(const std::string& thread_name, const std::string& context, const std::string& message);
    static void warn(const std::string& thread_name, const std::string& context, const std::string& message);
    static void error(const std::string& thread_name, const std::string& context, const std::string& message);

private:
    static LogLevel current_level;
    static std::string levelToString(LogLevel level);
    static std::string getTimestamp();
};

} // namespace core
} // namespace chess
