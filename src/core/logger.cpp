#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <mutex>

namespace chess {
namespace core {

LogLevel Logger::current_level = LogLevel::INFO;
static std::mutex log_mutex;

void Logger::init(LogLevel level) {
    current_level = level;
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

std::string Logger::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void Logger::log(LogLevel level, const std::string& thread_name, const std::string& context, const std::string& message) {
    if (level < current_level) return;

    std::lock_guard<std::mutex> lock(log_mutex);
    
    std::ostream& out = (level == LogLevel::ERROR) ? std::cerr : std::cout;
    
    out << "[" << getTimestamp() << "] "
        << "[" << levelToString(level) << "] "
        << "[" << thread_name << "] ";
        
    if (!context.empty()) {
        out << "[" << context << "] ";
    }
    
    out << message << std::endl;
}

void Logger::debug(const std::string& thread_name, const std::string& context, const std::string& message) {
    log(LogLevel::DEBUG, thread_name, context, message);
}

void Logger::info(const std::string& thread_name, const std::string& context, const std::string& message) {
    log(LogLevel::INFO, thread_name, context, message);
}

void Logger::warn(const std::string& thread_name, const std::string& context, const std::string& message) {
    log(LogLevel::WARN, thread_name, context, message);
}

void Logger::error(const std::string& thread_name, const std::string& context, const std::string& message) {
    log(LogLevel::ERROR, thread_name, context, message);
}

} // namespace core
} // namespace chess
