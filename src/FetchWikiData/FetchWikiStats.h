#pragma once

#include <string>
#include <vector>

// A struct to represent a wikipedia that can be downloaded
struct WikiEntry {
    std::string language_code;
    std::string language_name;
    std::string local_language_name;
    std::string wiki_id;
    int articles;
    int users;

    bool is_downloaded = false;
};

/**
 * @brief Fetch current Wikipedia statistics for available languages from Wikimedia.
 * @return Vector of `WikiEntry` records containing language and stats metadata
 */
std::vector<WikiEntry> fetch_wiki_stats();
