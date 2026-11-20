#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

struct DownloadProgress {
    uint64_t dlnow = 0;
    uint64_t dltotal = 0;  ///< 0 until the server has sent the size
    uint64_t dlspeed = 0;  ///< bytes per second
};

struct DownloadError {
    bool cancelled = false;  ///< stopped by a stop request (not a failure)
    std::string message;     ///< what went wrong, when not cancelled
};

/// Has a value when the complete file is at `path`.
using DownloadResult = std::expected<void, DownloadError>;

/**
 * @brief Download `url` to `path`, reporting progress.
 *
 * Data goes to "<path>.part", which is renamed to `path` only when the transfer completes (curl fails a transfer
 * that ends before the announced size), so an interrupted download never looks complete. A leftover .part file is
 * resumed with an HTTP range request. An existing `path` is taken as complete: a damaged dump is still caught when
 * the graph is built, since gzip checks a CRC-32 of the whole file.
 *
 * @param on_progress  called on this thread, at most every `refresh_rate`
 * @param stop         a stop request ends the transfer; the .part file is kept
 */
DownloadResult download_file(const std::string& url, const std::filesystem::path& path,
                             const std::function<void(const DownloadProgress&)>& on_progress,
                             std::chrono::milliseconds refresh_rate, std::stop_token stop = {});

/** @brief Database prefix used in dump URLs and file names for a language code ("zh-min-nan" -> "zh_min_nan"). */
std::string dump_db_prefix(std::string_view language_code);

/**
 * @brief Date (YYYYMMDD) of the newest dump run of `db` that has all four tables published.
 * @param stop  a stop request aborts the requests
 * @return the date, or why it could not be found
 */
std::expected<std::string, std::string> find_latest_dump(const std::string& db, std::stop_token stop = {});
