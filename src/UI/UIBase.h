#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ftxui/component/app.hpp>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FetchWikiData/DownloadWikiDump.h"
#include "FetchWikiData/FetchWikiStats.h"
#include "Graph/GraphBuilder.h"
#include "Graph/GraphStore.h"
#include "UI/BackgroundThreads.h"

namespace wg::app {

// UI stage enum
enum class UIStage : uint8_t {
    WikiSelection,
    Download,
    PrepareGraph,  // building the graph from the dumps (first run) or opening it
    UserInput,
    ShowPaths,
};

// The type of file in the wikipedia dump
enum class WikiFileType : uint8_t { Page, LinkTarget, PageLinks, Redirect };
inline constexpr size_t kWikiFileTypes = 4;

/// The type's token in dump file names, "<db>wiki-<date>-<token>.sql.gz"; indexed by WikiFileType
inline constexpr std::array<std::string_view, kWikiFileTypes> kWikiFileTokens = {"page", "linktarget", "pagelinks",
                                                                                 "redirect"};

/// The file type of a token in a dump file name ("pagelinks" -> PageLinks).
inline std::optional<WikiFileType> wiki_file_type(std::string_view token) {
    const auto it = std::ranges::find(kWikiFileTokens, token);
    if (it == kWikiFileTokens.end()) return std::nullopt;
    return static_cast<WikiFileType>(it - kWikiFileTokens.begin());
}

/// Dump file name, e.g. "enwiki-20261101-pagelinks.sql.gz" for ("en", "20261101", PageLinks).
inline std::string dump_file_name(std::string_view db, std::string_view date, WikiFileType type) {
    return std::format("{}wiki-{}-{}.sql.gz", db, date, kWikiFileTokens[std::to_underlying(type)]);
}

struct WikiFile {
    bool exists = false;
    std::filesystem::path data_path;
};

/// Structure representing a downloaded wiki with its metadata
struct DownloadedWiki {
    std::string language_code;  ///< database prefix, as in the file names ("en", "zh_min_nan")
    std::string date;
    std::array<WikiFile, kWikiFileTypes> files;  ///< indexed by WikiFileType; use file()

    [[nodiscard]] auto& file(this auto& self, WikiFileType type) { return self.files[std::to_underlying(type)]; }

    /// Page, link target and page links are needed; the redirect dump is optional (older downloads lack it)
    [[nodiscard]] bool complete() const {
        return file(WikiFileType::Page).exists && file(WikiFileType::LinkTarget).exists &&
               file(WikiFileType::PageLinks).exists;
    }

    [[nodiscard]] wg::DumpFiles dump_files() const {
        const WikiFile& redirect = file(WikiFileType::Redirect);
        return {file(WikiFileType::Page).data_path, file(WikiFileType::LinkTarget).data_path,
                file(WikiFileType::PageLinks).data_path,
                redirect.exists ? redirect.data_path : std::filesystem::path{}};
    }
};

/// The wiki list (UIStage::WikiSelection).
struct WikiListState {
    std::vector<WikiEntry> entries;  ///< every Wikipedia, from WikiStats; empty until fetched (or offline)
    bool loading = true;
    bool offline = false;            ///< the list could not be fetched
    std::vector<WikiEntry> fetched;  ///< written by the fetch thread before it sets `fetched_ready`
    std::atomic<bool> fetched_ready{false};
    int selected_index = 0;
    bool rescan = false;  ///< rescan the data folder before drawing the list again
};

/// The wiki chosen in the list.
struct SelectedWiki {
    std::string prefix;   ///< database prefix ("en", "zh_min_nan")
    std::string date;     ///< empty while a download is still looking for the latest dump
    DownloadedWiki wiki;  ///< its files, once they are all downloaded
};

/// Downloading the dumps (UIStage::Download).
struct DownloadState {
    struct File {
        std::atomic<DownloadProgress> progress{DownloadProgress{}};
        std::atomic<bool> complete{false};
    };
    std::array<File, kWikiFileTypes> files;  ///< indexed by WikiFileType; use file()
    std::string error;
    std::atomic<int> running{0};           ///< threads looking up the dump or downloading
    std::stop_source stop;                 ///< stopped on Esc, on exit, and by a download that fails
    std::atomic<bool> handed_over{false};  ///< the last download to finish starts the graph, once

    [[nodiscard]] File& file(WikiFileType type) { return files[std::to_underlying(type)]; }
};

/// Building the graph from the dumps if needed, then opening it (UIStage::PrepareGraph). The atomics are written by
/// the graph thread.
struct PrepareState {
    std::atomic<bool> running{false};
    std::atomic<bool> building{false};  ///< false while the graph is only being opened
    std::stop_source stop;
    std::atomic<wg::BuildProgress> progress{wg::BuildProgress{}};
    std::chrono::steady_clock::time_point started{};
    std::optional<wg::BuildReport> built;  ///< set if a build ran; written before the stage becomes UserInput
    std::string error;
};

/// The title inputs (UIStage::UserInput) and the search and its results (UIStage::ShowPaths).
struct SearchState {
    std::string start_title, end_title;
    int start_cursor = 0, end_cursor = 0;  ///< cursor positions in the two inputs (byte offsets)
    bool focus_start = true;               ///< give the start field the focus when the input is drawn next (new search)

    std::atomic<bool> running{false};
    std::atomic<wg::SearchProgress> progress{wg::SearchProgress{}};
    std::stop_source stop;
    std::string redirect_note;  ///< "USA redirects to United States", for titles that were redirects
    /// A problem with the titles, or written by the search thread before `running` goes false
    std::string error;
    wg::PathResult result;  ///< written by the search thread before `running` goes false
    int result_cursor = 0;  ///< highlighted path; the list scrolls to keep it visible
    static constexpr size_t kPathsListed = 1000;

    /// Forget the result (and any error), keeping the titles.
    void clear_result() {
        result = wg::PathResult{};
        result_cursor = 0;
        error.clear();
    }

    /// Start over: empty titles, or `start` typed into the start field, and no result.
    void new_search(std::string start = {}) {
        start_title = std::move(start);
        end_title.clear();
        start_cursor = static_cast<int>(start_title.size());
        end_cursor = 0;
        focus_start = true;
        clear_result();
    }
};

/// State shared by the UI thread and the background threads it starts.
///
/// Atomics may be touched from any thread. Plain members belong to the UI thread; background threads change them
/// only through post_to_ui(), except where a comment says otherwise.
///
/// Each kind of background work has a std::stop_source. The UI thread replaces it with a fresh one when it starts
/// that work, and requests a stop on Esc and on exit. A background thread gets a copy (or a token) when it is
/// started and never reads these members.
struct UIState {
    std::atomic<UIStage> stage{UIStage::WikiSelection};
    uint64_t ram_budget = wg::kBudgetAuto;  ///< for the next graph opened (--ram-budget, r/R in the wiki list)
    bool force_rebuild = false;             ///< --rebuild

    WikiListState wikis;
    SelectedWiki selected;
    DownloadState download;
    PrepareState prepare;
    SearchState search;

    /// Esc during a download, build or the title input: the cancel flags are set and the UI returns to the wiki list
    /// once nothing is running any more (see finish_going_back_if_idle).
    bool going_back = false;

    wg::GraphStore* graph = nullptr;
    std::stop_source app_stop;  ///< the UI is closing; never replaced
    BackgroundThreads threads;

    /// The UI is closing: stop all background work (then join `threads`).
    void stop_all() {
        app_stop.request_stop();
        download.stop.request_stop();
        prepare.stop.request_stop();
        search.stop.request_stop();
    }

    /// Redraw interval while something is in progress
    static constexpr std::chrono::milliseconds kRefreshRate{200};
};

/// Request a redraw from any thread (dropped if the UI is not running).
inline void post_ui_refresh() {
    if (auto* screen = ftxui::App::Active()) screen->PostEvent(ftxui::Event::Custom);
}

/// Run `task` on the UI thread, then redraw (dropped if the UI is not running).
inline void post_to_ui(std::function<void()> task) {
    if (auto* screen = ftxui::App::Active()) {
        screen->Post(std::move(task));
        screen->PostEvent(ftxui::Event::Custom);
    }
}

}  // namespace wg::app
