#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

namespace pmm {

class Logger {
public:
    static void init(const std::string& log_dir = "./logs",
                     const std::string& logger_name = "pmm_logger");
    static void updateSessionDir(const std::string& session_dir, const std::string& session_name);
    static std::shared_ptr<spdlog::logger> get();
    
private:
    static std::shared_ptr<spdlog::logger> logger_;
};

#define LOG_TRACE(...) pmm::Logger::get()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) pmm::Logger::get()->debug(__VA_ARGS__)
#define LOG_INFO(...)  pmm::Logger::get()->info(__VA_ARGS__)
#define LOG_WARN(...)  pmm::Logger::get()->warn(__VA_ARGS__)
#define LOG_ERROR(...) pmm::Logger::get()->error(__VA_ARGS__)
#define LOG_CRITICAL(...) pmm::Logger::get()->critical(__VA_ARGS__)

} // namespace pmm