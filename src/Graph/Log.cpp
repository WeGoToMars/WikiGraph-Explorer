#include "Graph/Log.h"

#include <atomic>
#include <string_view>

namespace wg {
namespace {
std::atomic<LogSink> g_sink{nullptr};
}  // namespace

void set_log_sink(LogSink sink) { g_sink.store(sink, std::memory_order_release); }

void log_message(LogLevel level, std::string_view message) {
    if (const LogSink sink = g_sink.load(std::memory_order_acquire)) sink(level, message);
}

}  // namespace wg
