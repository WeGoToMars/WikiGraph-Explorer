#pragma once

#include <memory>
#include <streambuf>
#include <string>

namespace wg::app {

/**
 * Log to log.txt in the logs folder (paths::logs_dir()), truncated on every run.
 *
 * The level is debug unless the SPDLOG_LEVEL environment variable says otherwise (e.g. SPDLOG_LEVEL=info).
 * @return empty on success, otherwise why the log file could not be opened (logging is then disabled)
 */
std::string init_logfile();

/// While alive, text written to std::cerr goes to the log (one warning per line) instead of the terminal.
///
/// For the terminal UI, which owns the screen: rapidgzip reports some decompression errors on std::cerr, and those
/// lines would be drawn over the UI. The error itself still reaches the UI as an exception.
class StderrToLog {
   public:
    StderrToLog();
    ~StderrToLog();
    StderrToLog(const StderrToLog&) = delete;
    StderrToLog& operator=(const StderrToLog&) = delete;

   private:
    class LineBuffer;
    std::unique_ptr<LineBuffer> buffer_;
    std::streambuf* previous_;
};

}  // namespace wg::app
