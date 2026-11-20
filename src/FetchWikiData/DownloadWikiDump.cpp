#include "DownloadWikiDump.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <regex>

#include "Http.h"

namespace fs = std::filesystem;

namespace {

std::unexpected<DownloadError> failed(std::string message) {
    return std::unexpected(DownloadError{.cancelled = false, .message = std::move(message)});
}

}  // namespace

DownloadResult download_file(const std::string& url, const fs::path& path,
                             const std::function<void(const DownloadProgress&)>& on_progress,
                             std::chrono::milliseconds refresh_rate, std::stop_token stop) {
    auto complete = [&] {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (on_progress) on_progress({.dlnow = size, .dltotal = size, .dlspeed = 0});
        return DownloadResult{};
    };
    std::error_code ec;
    if (fs::exists(path, ec)) return complete();

    const fs::path part = path.string() + ".part";
    const uint64_t offset = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    cpr::Header headers;
    if (offset > 0) {
        spdlog::info("Resuming {} from byte {}", path.string(), offset);
        headers["Range"] = std::format("bytes={}-", offset);
    }

    long status = 0;         // of the response being received (the last one, after redirects)
    uint64_t base = offset;  // bytes on disk before this response
    std::ofstream file;
    auto last_report = std::chrono::steady_clock::now();
    uint64_t last_now = 0;
    const cpr::Response r = wiki_http::get(
        cpr::Url{url}, headers, wiki_http::user_agent(), cpr::ConnectTimeout{wiki_http::kConnectTimeout},
        cpr::LowSpeed{1, wiki_http::kStallTimeout},
        // Status lines look like "HTTP/1.1 206 Partial Content" or "HTTP/2 200"
        cpr::HeaderCallback([&](const std::string_view& line, intptr_t) {
            if (const size_t sp = line.find(' '); line.starts_with("HTTP/") && sp != std::string_view::npos)
                std::from_chars(line.data() + sp + 1, line.data() + line.size(), status);
            return true;
        }),
        cpr::WriteCallback([&](const std::string_view& data, intptr_t) {
            if (!file.is_open()) {
                if (status != 200 && status != 206) return false;  // an error page: never write it into the dump
                if (status == 200) base = 0;                       // the whole file after all: start over
                file.open(part, std::ios::binary | (status == 206 ? std::ios::app : std::ios::trunc));
            }
            return static_cast<bool>(file.write(data.data(), static_cast<std::streamsize>(data.size())));
        }),
        cpr::ProgressCallback([&](cpr::cpr_off_t total, cpr::cpr_off_t now, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
            if (stop.stop_requested()) return false;
            const auto t = std::chrono::steady_clock::now();
            const std::chrono::duration<double> dt = t - last_report;
            const auto received = static_cast<uint64_t>(now);
            if (!on_progress || dt < refresh_rate || received <= last_now) return true;
            on_progress({.dlnow = base + received,
                         .dltotal = total > 0 ? base + static_cast<uint64_t>(total) : 0,
                         .dlspeed = static_cast<uint64_t>(static_cast<double>(received - last_now) / dt.count())});
            last_report = t;
            last_now = received;
            return true;
        }));
    const bool write_failed = file.fail();
    file.close();

    if (stop.stop_requested()) return std::unexpected(DownloadError{.cancelled = true, .message = {}});
    if (write_failed) return failed(std::format("cannot write {} (is the disk full?)", part.string()));
    // 416 to a range request: the .part file already holds everything (the app stopped before renaming it)
    if (status != 416 || offset == 0) {
        if (status == 0) return failed(std::format("cannot reach {}: {}", url, r.error.message));
        if (status != 200 && status != 206) return failed(std::format("{} answered HTTP {}", url, status));
        if (r.error) return failed(std::format("download of {} failed: {}", url, r.error.message));
    }
    fs::rename(part, path, ec);
    if (ec) return failed(std::format("cannot rename {}: {}", part.string(), ec.message()));
    spdlog::info("Downloaded {}", path.string());
    return complete();
}

std::string dump_db_prefix(std::string_view language_code) {
    // Database names use underscores where language codes use hyphens (zh-min-nan -> zh_min_nanwiki), and one wiki
    // kept its old name when its code changed
    if (language_code == "be-tarask") return "be_x_old";
    std::string db(language_code);
    std::ranges::replace(db, '-', '_');
    return db;
}

std::expected<std::string, std::string> find_latest_dump(const std::string& db, std::stop_token stop) {
    // The prefix comes from a downloaded list and ends up in file names
    if (db.empty() || !std::ranges::all_of(db, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
        return std::unexpected(std::format("'{}' is not a valid database name", db));
    std::string date;
    // Tables of one dump run are published days apart, so the page table of a run in progress can exist before its
    // pagelinks table does. Use the newest run that has all four: the oldest of the four "latest" dates.
    for (const char* table : {"page", "pagelinks", "linktarget", "redirect"}) {
        const std::string rss =
            std::format("https://dumps.wikimedia.org/{}wiki/latest/{}wiki-latest-{}.sql.gz-rss.xml", db, db, table);
        const cpr::Response r = wiki_http::get(cpr::Url{rss}, wiki_http::user_agent(),
                                               cpr::Timeout{wiki_http::kRequestTimeout},
                                               wiki_http::cancel_callback(stop));
        // The feed's <link> is the dump directory: https://dumps.wikimedia.org/<db>wiki/<date>
        std::smatch m;
        if (r.status_code != 200 || !std::regex_search(r.text, m, std::regex(R"(<link>[^<]*/(\d{8})/?</link>)"))) {
            auto why = r.status_code == 0 ? std::format("cannot reach {}: {}", rss, r.error.message)
                                          : std::format("no dump date in {} (HTTP {})", rss, r.status_code);
            spdlog::error("{}", why);
            return std::unexpected(std::move(why));
        }
        if (date.empty() || m[1].str() < date) date = m[1].str();
    }
    spdlog::debug("Latest dump of {}wiki with all tables: {}", db, date);
    return date;
}
