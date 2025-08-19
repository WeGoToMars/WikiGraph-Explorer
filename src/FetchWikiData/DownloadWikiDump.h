
#include <atomic>
#include <string>

#include "../UI/UIBase.h"

/**
 * @brief Container for dump URLs of a specific wiki and date.
 */
struct DownloadURLs {
    std::string date;
    std::string page;
    std::string pagelinks;
    std::string linktarget;
};

/** @brief Download a file to disk while updating a progress struct. */
void download_file(std::string url, std::string output_filename, std::atomic<UIState::DownloadProgress>& dp,
                   std::chrono::milliseconds refresh_rate);
/** @brief Resolve dump URLs for a wiki prefix by reading the RSS feed. */
DownloadURLs get_urls_from_rss(std::string& wiki_prefix);
