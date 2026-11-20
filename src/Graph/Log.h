#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

// Logging for the core, with no dependency: the host installs a sink (the terminal UI forwards to spdlog, the Mac app
// to os.Logger). Messages are discarded until a sink is installed.
namespace wg {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

/// Receives every message; called on whichever thread logged it, so it must be thread-safe.
using LogSink = void (*)(LogLevel level, std::string_view message);

/// Install the sink (null discards messages).
void set_log_sink(LogSink sink);
void log_message(LogLevel level, std::string_view message);

template <class... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
    log_message(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
    log_message(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    log_message(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
    log_message(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace wg
