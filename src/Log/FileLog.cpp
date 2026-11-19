#include "FileLog.h"

#include <spdlog/cfg/env.h>
#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>

#include "Graph/Log.h"
#include "Utils/PathUtils.h"

namespace wg::app {

// Collects what is written to it and logs each complete line. Several threads may write to std::cerr at once.
class StderrToLog::LineBuffer : public std::streambuf {
   public:
    ~LineBuffer() override;

   protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const char c = traits_type::to_char_type(ch);
        xsputn(&c, 1);
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        const std::lock_guard lk(mutex_);
        for (const char c : std::string_view(s, static_cast<size_t>(n))) {
            if (c == '\n') {
                spdlog::warn("{}", line_);
                line_.clear();
            } else {
                line_ += c;
            }
        }
        return n;
    }

   private:
    void flush_partial() {
        const std::lock_guard lk(mutex_);
        if (!line_.empty()) spdlog::warn("{}", line_);
        line_.clear();
    }

    std::mutex mutex_;
    std::string line_;
};

StderrToLog::LineBuffer::~LineBuffer() { flush_partial(); }

StderrToLog::StderrToLog() : buffer_(std::make_unique<LineBuffer>()), previous_(std::cerr.rdbuf(buffer_.get())) {}

StderrToLog::~StderrToLog() { std::cerr.rdbuf(previous_); }

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
