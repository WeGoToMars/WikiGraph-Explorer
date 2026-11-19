#pragma once

#include <string>

namespace wg::app {

/**
 * Log to log.txt in the logs folder (paths::logs_dir()), truncated on every run.
 *
 * The level is debug unless the SPDLOG_LEVEL environment variable says otherwise (e.g. SPDLOG_LEVEL=info).
 * @return empty on success, otherwise why the log file could not be opened (logging is then disabled)
 */
std::string init_logfile();

}  // namespace wg::app
