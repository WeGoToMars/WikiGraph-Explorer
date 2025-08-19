#include "DownloadWikiDump.h"

#include <cpr/cpr.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <regex>

#include "spdlog/spdlog.h"

void download_file(std::string url, std::string output_filename, std::atomic<UIState::DownloadProgress>& dp,
                   std::chrono::milliseconds refresh_rate) {
    auto last_refresh = std::chrono::steady_clock::now();
    uint64_t last_dlnow = 0;

    cpr::Response r =
        cpr::Get(cpr::Url{url},
                 // https://docs.libcpr.org/advanced-usage.html#:~:text=ProgressCallback
                 cpr::ProgressCallback([&](cpr::cpr_off_t dltotal_, cpr::cpr_off_t dlnow_, cpr::cpr_off_t ultotal,
                                           cpr::cpr_off_t ulnow, intptr_t userdata) -> bool {
                     // Unused parameters for upload and userdata
                     (void)userdata;
                     (void)ultotal;
                     (void)ulnow;

                     // Cast cpr_off_t to uint64_t
                     auto dlnow = static_cast<uint64_t>(dlnow_);
                     auto dltotal = static_cast<uint64_t>(dltotal_);

                     // This callback is called every time a chunk of data is received, which is very frequent.
                     // We only want to update the UI every refresh_rate milliseconds.
                     auto now = std::chrono::steady_clock::now();
                     std::chrono::duration<double, std::milli> dt = now - last_refresh;
                     if (dt < refresh_rate || dlnow <= last_dlnow) {
                         return true;
                     }

                     last_refresh = now;

                     // Calculate download speed in bytes/s
                     auto dlspeed = static_cast<uint64_t>((dlnow - last_dlnow) / (dt.count() / 1000.0));
                     dp.store({.dlnow = dlnow, .dltotal = dltotal, .dlspeed = dlspeed});

                     last_dlnow = dlnow;

                     post_ui_refresh();

                     return true;
                 }),
                 // https://docs.libcpr.org/advanced-usage.html#:~:text=WriteCallback
                 cpr::WriteCallback([&](const std::string_view& data, intptr_t userdata) -> bool {
                     // Unused parameter userdata
                     (void)userdata;

                     // Append data to the output file
                     std::ofstream file(output_filename, std::ios::app | std::ios::binary);
                     file.write(data.data(), static_cast<std::streamsize>(data.size()));
                     file.flush();

                     return true;
                 }));

    if (r.status_code != 200) {
        spdlog::error("Failed to download file: {}", url);
        return;
    }

    spdlog::info("Download complete: {}", output_filename);
}

DownloadURLs get_urls_from_rss(std::string& wiki_prefix) {
    // Make a request to the RSS feed to get the latest dump date
    std::string rss_url = std::format("https://dumps.wikimedia.org/{}wiki/latest/{}wiki-latest-page.sql.gz-rss.xml",
                                      wiki_prefix, wiki_prefix);

    cpr::Response r = cpr::Get(cpr::Url{rss_url});
    if (r.status_code != 200) {
        spdlog::error("Failed to fetch RSS from: {}", rss_url);
        return {};
    }

    // Extract the link from XML tag <link>...</link>
    std::smatch match;
    std::regex_search(r.text, match, std::regex("<link>([^<]+)</link>"));
    std::string base_url = match[1].str();

    std::string date = base_url.substr(base_url.find_last_of('/') + 1);
    spdlog::debug("Latest available dump date: {}", date);

    DownloadURLs urls;
    urls.page = std::format("https://dumps.wikimedia.org/{}wiki/{}/{}wiki-{}-page.sql.gz", wiki_prefix, date,
                            wiki_prefix, date);
    urls.pagelinks = std::format("https://dumps.wikimedia.org/{}wiki/{}/{}wiki-{}-pagelinks.sql.gz", wiki_prefix, date,
                                 wiki_prefix, date);
    urls.linktarget = std::format("https://dumps.wikimedia.org/{}wiki/{}/{}wiki-{}-linktarget.sql.gz", wiki_prefix,
                                  date, wiki_prefix, date);
    urls.date = date;

    spdlog::debug("Download URL page: {}", urls.page);
    spdlog::debug("Download URL pagelinks: {}", urls.pagelinks);
    spdlog::debug("Download URL linktarget: {}", urls.linktarget);

    return urls;
}
