#include "utils/logger.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <vector>
#include <filesystem>

namespace pmm {

std::shared_ptr<spdlog::logger> Logger::logger_;

void Logger::init(const std::string& log_dir, const std::string& session_name) {
    std::filesystem::create_directories(log_dir);
    
    std::vector<spdlog::sink_ptr> sinks;
    
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    sinks.push_back(console_sink);
    
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/" + session_name + "_all.log", 
        1024 * 1024 * 10,
        5
    );
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    sinks.push_back(file_sink);
    
    auto error_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/" + session_name + "_errors.log",
        1024 * 1024 * 10,
        3
    );
    error_sink->set_level(spdlog::level::err);
    error_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    sinks.push_back(error_sink);
    
    logger_ = std::make_shared<spdlog::logger>("pmm", sinks.begin(), sinks.end());
    logger_->set_level(spdlog::level::trace);
    logger_->flush_on(spdlog::level::warn);
    
    spdlog::register_logger(logger_);
    spdlog::set_default_logger(logger_);

    LOG_INFO("Logger initialized - session: {}, log_dir: {}", session_name, log_dir);
}

void Logger::updateSessionDir(const std::string& session_dir, const std::string& session_name) {
    if (!logger_) {
        LOG_WARN("Logger not initialized, cannot update session directory");
        return;
    }
    
    std::filesystem::create_directories(session_dir);
    
    // Drop existing logger and recreate with new session directory
    spdlog::drop("pmm");
    
    std::vector<spdlog::sink_ptr> sinks;
    
    // Console sink (unchanged)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::info);
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    sinks.push_back(console_sink);
    
    // File sink - now in session directory
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        session_dir + "/" + session_name + "_all.log", 
        1024 * 1024 * 10,
        5
    );
    file_sink->set_level(spdlog::level::trace);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    sinks.push_back(file_sink);
    
    // Error sink - now in session directory
    auto error_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        session_dir + "/" + session_name + "_errors.log",
        1024 * 1024 * 10,
        3
    );
    error_sink->set_level(spdlog::level::err);
    error_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    sinks.push_back(error_sink);
    
    logger_ = std::make_shared<spdlog::logger>("pmm", sinks.begin(), sinks.end());
    logger_->set_level(spdlog::level::trace);
    logger_->flush_on(spdlog::level::warn);
    
    spdlog::register_logger(logger_);
    spdlog::set_default_logger(logger_);
    
    LOG_INFO("Logger updated - session logs now in: {}", session_dir);
}

std::shared_ptr<spdlog::logger> Logger::get() {
    if (!logger_) {
        init("./logs", "pmm_logger");
    }
    return logger_;
}

} // namespace pmm