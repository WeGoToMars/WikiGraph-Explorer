#include "FileLog.h"

#include <filesystem>
#include <fstream>

#include "Utils/PathUtils.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

void init_logfile() {
    try {
        PathUtils::ensure_logs_dir_exists();
        auto log_path = PathUtils::get_logs_dir() / "log.txt";

        // Clear the log file if it exists
        if (std::filesystem::exists(log_path)) {
            std::ofstream file(log_path, std::ios::trunc);
            file.close();
        }

        auto logger = spdlog::basic_logger_mt("logger", log_path.string());
        spdlog::set_default_logger(logger);

        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_every(std::chrono::seconds(1));
        spdlog::debug("Logging initialized successfully");
    } catch (const spdlog::spdlog_ex& ex) {
        // Log init failed
        throw std::runtime_error("Failed to initialize log file: " + std::string(ex.what()));
    }
}
