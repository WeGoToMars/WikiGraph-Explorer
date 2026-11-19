#include "FileLog.h"

#include <spdlog/cfg/env.h>
#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <memory>

#include "Graph/Log.h"
#include "Utils/PathUtils.h"

namespace wg::app {

namespace {

// The core logs through wg::log; forward it to spdlog
void forward_to_spdlog(wg::LogLevel level, std::string_view message) {
    switch (level) {
        case wg::LogLevel::Debug:
            spdlog::debug("{}", message);
            break;
        case wg::LogLevel::Info:
            spdlog::info("{}", message);
            break;
        case wg::LogLevel::Warn:
            spdlog::warn("{}", message);
            break;
        case wg::LogLevel::Error:
            spdlog::error("{}", message);
            break;
    }
}

}  // namespace

std::string init_logfile() {
    const auto log_path = paths::logs_dir() / "log.txt";
    std::string error;
    try {
        // truncate = true: start a fresh log on every run
        spdlog::set_default_logger(spdlog::basic_logger_mt("logger", log_path.string(), true));
    } catch (const spdlog::spdlog_ex& ex) {
        // Logging is not essential: keep running without it rather than refusing to start
        error = std::string("cannot write the log file ") + log_path.string() + ": " + ex.what();
        spdlog::set_default_logger(spdlog::null_logger_mt("logger"));
    }
    spdlog::set_level(spdlog::level::debug);
    spdlog::cfg::load_env_levels();  // SPDLOG_LEVEL=info (or warn, ...) overrides the level
    spdlog::flush_every(std::chrono::seconds(1));
    wg::set_log_sink(forward_to_spdlog);
    spdlog::debug("Logging initialized");
    return error;
}

}  // namespace wg::app
