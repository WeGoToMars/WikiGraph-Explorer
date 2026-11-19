#pragma once

#include <stop_token>
#include <string>
#include <vector>

// A struct to represent a wikipedia that can be downloaded
struct WikiEntry {
    std::string language_code;  ///< as on wikipedia.org ("en", "zh-min-nan"); see dump_db_prefix for dump names
    std::string language_name;
    std::string local_language_name;
    int articles = 0;
    int users = 0;
};

/**
 * @brief Fetch current Wikipedia statistics for available languages from Wikimedia.
 * @param stop  aborts the request when a stop is requested
 * @return Vector of `WikiEntry` records containing language and stats metadata; empty when offline
 */
std::vector<WikiEntry> fetch_wiki_stats(std::stop_token stop = {});
