#include "UI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "FetchWikiData/DownloadWikiDump.h"
#include "FetchWikiData/FetchWikiStats.h"
#include "Graph/Budget.h"
#include "Graph/GraphBuilder.h"
#include "Graph/GraphStore.h"
#include "Graph/PathSearch.h"
#include "Graph/Platform.h"
#include "Graph/TitleIndex.h"
#include "UI/UIBase.h"
#include "Utils/PathUtils.h"
#include "Utils/ScopeExit.h"
#include "Utils/StringUtils.h"
#include "WikiSelectUI.h"
#include "spdlog/spdlog.h"

using namespace ftxui;

namespace wg::app {

// ------------------------------------------------------------------------------------------------ Utility functions

namespace {
/// Rows Page Up and Page Down move the highlighted path
constexpr int kPageRows = 10;

// Create formatted text with optional color
Element create_text(const std::string& content, bool bold = false, Color color = Color::Default) {
    auto elem = text(content);
    if (bold) elem = elem | ftxui::bold;
    if (color != Color::Default) elem = elem | ftxui::color(color);
    return elem;
}

Element hint(const std::string& content) { return text(content) | color(Color::GrayDark); }

// Create step header
Element create_step_header(const std::string& step, const std::string& description) {
    return text("[" + step + "] " + description) | bold;
}

std::string gb(uint64_t bytes) { return std::format("{:.2f} GB", static_cast<double>(bytes) / wg::kBytesPerGB); }

/// "s" unless there is exactly one.
const char* plural(uint64_t n) { return n == 1 ? "" : "s"; }

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

/// Run `job` on a thread owned by state.threads, which logs an exception that leaves it. When the job ends, a
/// pending "go back" may be able to complete.
void spawn(UIState& state, std::function<void()> job) {
    state.threads.start([&state, task = std::move(job)] {
        const ScopeExit check_going_back{[&state] { post_to_ui([&state] { finish_going_back_if_idle(state); }); }};
        task();
    });
}

/// Calls `refresh` at most once per UIState::kRefreshRate: for progress callbacks, which come much more often.
class Throttle {
   public:
    bool due() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ < UIState::kRefreshRate) return false;
        last_ = now;
        return true;
    }

   private:
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};
}  // namespace

// ------------------------------------------------------------------------------------------------ Main ui entry point

void run_ui(UIState& state) {
    // Use fullscreen for all stages for consistent behavior
    App screen = App::Fullscreen();

    // The list of Wikipedias takes an HTTP request: show the downloaded wikis meanwhile. The wiki list picks the
    // result up when it is drawn next (a refresh posted before the loop starts would be dropped, the flag is not).
    state.threads.start([&state, stop = state.app_stop.get_token()] {
        state.wikis.fetched = fetch_wiki_stats(stop);
        state.wikis.fetched_ready = true;
        post_ui_refresh();
    });

    // Create components
    Component wiki_select_ui = create_wiki_select_ui(state);
    Component download_ui = create_download_ui(state);
    Component progress_ui = create_progress_ui(state);
    Component input_ui = create_input_ui(state);
    Component results_ui = create_results_ui(state);
    Component main_ui = create_main_ui(wiki_select_ui, download_ui, input_ui, results_ui, progress_ui, state);

    // Keys that apply across stages see every event first
    auto main_with_global_events =
        CatchEvent(main_ui, [&](const Event& event) { return handle_key_events(event, state); });

    screen.Loop(main_with_global_events);

    // The UI is gone: stop the background work and wait for it, so no thread outlives `state` or the graph.
    // Partial downloads stay as .part files and resume next time; an unfinished graph build is removed.
    state.stop_all();
    if (state.threads.running() > 0) std::fputs("Stopping background work...\n", stderr);
    state.threads.join_all();
}

Component create_main_ui(Component& wiki_select_ui, Component& download_ui, Component& input_ui, Component& results_ui,
                         Component& progress_ui, UIState& state) {
    // Create a renderer that switches between components based on stage
    auto main_ui = Renderer([&] {
        switch (state.stage.load()) {
            case UIStage::WikiSelection:
                return center(wiki_select_ui->Render()) | flex;
            case UIStage::Download:
                return download_ui->Render();
            case UIStage::PrepareGraph:
                return progress_ui->Render();
            case UIStage::UserInput:
                return vbox({progress_ui->Render(), input_ui->Render()});
            case UIStage::ShowPaths:
                return vbox({progress_ui->Render(), results_ui->Render() | flex});
        }
        return text("");
    });

    // Delegate events to the component of the current stage
    auto main_with_events = CatchEvent(main_ui, [&](const Event& event) {
        switch (state.stage.load()) {
            case UIStage::WikiSelection:
                return wiki_select_ui->OnEvent(event);
            case UIStage::Download:
                return download_ui->OnEvent(event);
            case UIStage::UserInput:
                return input_ui->OnEvent(event);
            case UIStage::ShowPaths:
                return results_ui->OnEvent(event);
            case UIStage::PrepareGraph:
                return false;
        }
        return false;
    });

    return main_with_events;
}

// ------------------------------------------------------------------------------------------------ Going back to the
// wiki list

namespace {
void reset_downloads(UIState& state) {
    for (auto& f : state.download.files) {
        f.progress = DownloadProgress{};
        f.complete = false;
    }
    state.download.error.clear();
    state.download.stop = std::stop_source{};
    state.download.handed_over = false;
}
}  // namespace

void finish_going_back_if_idle(UIState& state) {
    if (!state.going_back) return;
    if (state.download.running > 0 || state.prepare.running || state.search.running) return;  // still stopping
    state.going_back = false;
    if (state.graph != nullptr) state.graph->close();
    reset_downloads(state);
    state.prepare.error.clear();
    state.search.new_search();
    state.selected.wiki = DownloadedWiki{};
    state.selected.date.clear();
    state.wikis.rescan = true;  // a wiki downloaded just now should show as downloaded
    state.stage = UIStage::WikiSelection;
}

// ------------------------------------------------------------------------------------------------ Download stage

namespace {
bool all_downloads_complete(const UIState& state) {
    return std::ranges::all_of(state.download.files, [](const DownloadState::File& f) { return f.complete.load(); });
}

DownloadedWiki describe_downloaded(const std::string& prefix, const std::string& date) {
    DownloadedWiki wiki;
    wiki.language_code = prefix;
    wiki.date = date;
    for (size_t i = 0; i < kWikiFileTypes; ++i) {
        WikiFile& f = wiki.files[i];
        f.data_path = paths::data_dir() / dump_file_name(prefix, date, static_cast<WikiFileType>(i));
        std::error_code ec;
        f.exists = std::filesystem::exists(f.data_path, ec);
    }
    return wiki;
}

/// The download stage's work was stopped (Esc, a failed download) or the UI has moved on.
bool download_abandoned(const UIState& state) {
    return state.going_back || state.download.stop.stop_requested() || state.stage != UIStage::Download;
}

/// UI thread: every file is downloaded.
void downloads_finished(UIState& state) {
    if (download_abandoned(state)) return;
    state.selected.wiki = describe_downloaded(state.selected.prefix, state.selected.date);
    start_preparing_graph(state);
}

/// Background thread: download one file. `stop` is this download's: a failure stops the other files too.
void download_one(UIState& state, WikiFileType type, const std::string& url, const std::filesystem::path& path,
                  std::stop_source stop) {
    auto& file = state.download.file(type);
    const DownloadResult res = download_file(
        url, path,
        [&file](const DownloadProgress& p) {
            file.progress = p;
            post_ui_refresh();
        },
        UIState::kRefreshRate, stop.get_token());
    if (res) {
        file.complete = true;
        if (all_downloads_complete(state) && !state.download.handed_over.exchange(true))
            post_to_ui([&state] { downloads_finished(state); });
    } else if (!res.error().cancelled) {
        spdlog::error("Download failed: {}", res.error().message);
        stop.request_stop();  // stop the other files; the user can retry, and finished parts are kept
        post_to_ui([&state, msg = res.error().message] {
            if (state.download.error.empty()) state.download.error = msg;
        });
    }
}

/// UI thread: the dump run is known (or could not be found), start the file downloads.
void start_file_downloads(UIState& state, const std::expected<std::string, std::string>& date) {
    if (download_abandoned(state)) return;
    if (!date) {
        state.download.error = "Could not find a complete dump of this wiki: " + date.error();
        return;
    }
    state.selected.date = *date;

    auto job_for = [&](WikiFileType type) {
        const std::string& db = state.selected.prefix;
        const std::string name = dump_file_name(db, *date, type);
        return [&state, type, url = std::format("https://dumps.wikimedia.org/{}wiki/{}/{}", db, *date, name),
                path = paths::data_dir() / name,
                stop = state.download.stop] { download_one(state, type, url, path, stop); };
    };
    auto run = [&state](std::function<void()> job) {
        ++state.download.running;
        spawn(state, [&state, task = std::move(job)] {
            const ScopeExit running{[&state] { --state.download.running; }};
            task();
        });
    };
    run(job_for(WikiFileType::Page));
    run(job_for(WikiFileType::PageLinks));
    // The two smaller files share one connection, so at most three run at once (Wikimedia's per-IP limit)
    auto linktarget = job_for(WikiFileType::LinkTarget);
    auto redirect = job_for(WikiFileType::Redirect);
    run([linktarget, redirect] {
        linktarget();
        redirect();
    });
}

Element render_download_progress(const DownloadProgress& p) {
    const float ratio = p.dltotal > 0 ? static_cast<float>(p.dlnow) / static_cast<float>(p.dltotal) : 0.0f;
    const std::string sizes = std::format("{:7.2f} MB / {:7.2f} MB", static_cast<double>(p.dlnow) / wg::kBytesPerMB,
                                          static_cast<double>(p.dltotal) / wg::kBytesPerMB);
    return hbox({gauge(ratio) | flex, text(" "), text(sizes), text(" | "),
                 text(std::format("{:5.2f} MB/s", static_cast<double>(p.dlspeed) / wg::kBytesPerMB))});
}

/// A box with a title and one line of status under it.
Element status_box(const std::string& title, const std::string& status, Color color) {
    return vbox({create_text(title, true), separator(), create_text(status, false, color)}) | border;
}

Element render_download_ui(UIState& state) {
    const std::string wiki = std::format("{}wiki", state.selected.prefix);
    if (!state.download.error.empty()) {
        return vbox({create_text("Download Error", true), separator(),
                     create_text(state.download.error, false, Color::Red), separator(),
                     state.download.running > 0
                         ? text("Stopping the other downloads...")
                         : text("Press any key to return to the wiki list. Finished files and partial "
                                "downloads are kept, so selecting the wiki again resumes.")}) |
               border;
    }
    if (state.going_back)
        return status_box("Downloading " + wiki, "Stopping the downloads (partial files are kept)...", Color::Yellow);
    if (state.selected.date.empty())
        return vbox({create_text("Downloading " + wiki, true), separator(),
                     create_text("Finding the latest complete dump...", false, Color::Yellow), separator(),
                     hint("Esc: back to the wiki list")}) |
               border;

    double total_speed = 0;
    for (const auto& f : state.download.files) total_speed += static_cast<double>(f.progress.load().dlspeed);

    const std::string url = std::format("https://dumps.wikimedia.org/{}/{}/", wiki, state.selected.date);
    Elements elements;
    elements.push_back(hbox({create_text(std::format("Downloading {} from ", wiki), true),
                             create_text(url, true, Color::GrayDark) | hyperlink(url),
                             create_text(std::format(" at {:.2f} MB/s", total_speed / wg::kBytesPerMB), true)}));
    elements.push_back(separator());
    for (const auto& [label, type] :
         {std::pair{"Page file:", WikiFileType::Page}, std::pair{"Page links file:", WikiFileType::PageLinks},
          std::pair{"Link target file:", WikiFileType::LinkTarget},
          std::pair{"Redirect file:", WikiFileType::Redirect}}) {
        elements.push_back(text(label));
        elements.push_back(render_download_progress(state.download.file(type).progress.load()));
    }
    elements.push_back(separator());
    elements.push_back(hint("Esc: back to the wiki list (partial downloads are kept and resume next time)"));
    return vbox(std::move(elements)) | border;
}
}  // namespace

void start_download(UIState& state) {
    reset_downloads(state);
    state.selected.date.clear();
    state.going_back = false;
    state.stage = UIStage::Download;

    // Finding the dump takes a few HTTP requests: make them off the UI thread
    const std::string db = state.selected.prefix;
    spdlog::debug("Downloading {}wiki", db);
    ++state.download.running;
    spawn(state, [&state, db, stop = state.download.stop.get_token()] {
        const ScopeExit running{[&state] { --state.download.running; }};
        auto date = find_latest_dump(db, stop);
        post_to_ui([&state, found = std::move(date)] { start_file_downloads(state, found); });
    });

    // Redraw regularly while downloading, so the speed stays current even when a transfer stalls
    spawn(state, [&state, stop = state.app_stop.get_token()] {
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock lk(m);
        // Sleeps between redraws; a stop request (the UI closing) ends the sleep, and the loop, at once
        while (state.stage == UIStage::Download) {
            if (cv.wait_for(lk, stop, UIState::kRefreshRate, [&stop] { return stop.stop_requested(); })) return;
            post_ui_refresh();
        }
    });
}

Component create_download_ui(UIState& state) {
    return Renderer(Container::Vertical({}), [&state] { return render_download_ui(state); });
}

// ------------------------------------------------------------------------------------------------ Graph preparation
// (build or open)

namespace {
/// Graph thread: build the graph if it is missing or out of date, then open it.
void prepare_graph(UIState& state, const wg::DumpFiles& dumps, std::stop_token stop) {
    PrepareState& prepare = state.prepare;
    const ScopeExit done{[&prepare] { prepare.running = false; }};
    try {
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
        Throttle refresh;
        auto on_progress = [&](const wg::BuildProgress& p) {
            prepare.building = true;
            prepare.progress = p;
            if (refresh.due()) post_ui_refresh();
        };
        wg::BuildReport report;
        if (wg::ensure_graph(dumps, state.force_rebuild, threads, on_progress, &report, stop)) prepare.built = report;
        prepare.building = false;
        post_ui_refresh();
        if (stop.stop_requested()) return;
        state.graph->open(wg::graph_dir_for(dumps), state.ram_budget);
        post_to_ui([&state] {
            if (!state.going_back) state.stage = UIStage::UserInput;  // otherwise going back closes the graph
        });
    } catch (const wg::BuildCancelled&) {
        // Esc or exit; going back (or run_ui) takes it from here
        spdlog::info("Graph build cancelled");
    } catch (const std::exception& e) {
        spdlog::error("Preparing the graph failed: {}", e.what());
        post_to_ui([&state, msg = std::string(e.what())] { state.prepare.error = msg; });
    }
}

Element render_build_steps(const UIState& state, const std::string& wiki) {
    const wg::BuildProgress p = state.prepare.progress.load();
    const bool has_redirects = state.selected.wiki.file(WikiFileType::Redirect).exists;
    const int current = static_cast<int>(p.step);
    Elements elements = {create_step_header(std::format("Step {}/{}", current + 1, wg::kBuildSteps),
                                            "Building the link graph of " + wiki + " (first run only)"),
                         separator()};
    for (int i = 0; i < wg::kBuildSteps; ++i) {
        const auto step = static_cast<wg::BuildStep>(i);
        if (step == wg::BuildStep::Redirects && !has_redirects) continue;
        const char* mark = i < current ? "[x] " : i == current ? "[>] " : "[ ] ";
        elements.push_back(create_text(std::string(mark) + wg::build_step_name(step), i == current,
                                       i < current ? Color(Color::Green) : Color(Color::Default)));
    }
    const float ratio = p.total > 0 ? static_cast<float>(p.done) / static_cast<float>(p.total) : 0.0f;
    elements.push_back(text(" "));
    elements.push_back(hbox({text(std::format("{:5.1f}% ", ratio * 100)), gauge(ratio) | flex}));
    if (p.step != wg::BuildStep::Finish) elements.push_back(text(std::format("Rows parsed: {:L}", p.rows)));
    elements.push_back(hint(std::format("Elapsed: {:.1f} s | memory in use: {}", seconds_since(state.prepare.started),
                                        gb(wg::process_footprint_bytes()))));
    if (!has_redirects)
        elements.push_back(
            create_text("No redirect dump for this wiki: links through redirects are dropped.", false, Color::Yellow));
    elements.push_back(hint("Esc: cancel and go back to the wiki list (the build starts over next time)"));
    return vbox(std::move(elements)) | border;
}

/// While the graph is built or opened.
Element render_preparing(const UIState& state) {
    const std::string wiki = std::format("{}wiki ({})", state.selected.wiki.language_code, state.selected.wiki.date);
    if (state.going_back)
        return vbox({create_step_header("Stopping", wiki), separator(),
                     create_text("Stopping the graph build...", false, Color::Yellow)}) |
               border;
    if (state.prepare.building) return render_build_steps(state, wiki);
    return vbox({create_step_header("Opening", "Loading the graph of " + wiki), separator(),
                 text("RAM budget: " + wg::budget_to_string(state.ram_budget)),
                 create_text("Loading...", false, Color::Yellow)}) |
           border;
}

/// Above the search once the graph is open: its size and how it is held in RAM.
Element render_graph_ready(const UIState& state) {
    const wg::GraphInfo& info = state.graph->info();
    const std::string mode =
        info.all_in_ram
            ? std::format("all {} of links in RAM, no disk reads during search", gb(info.list_bytes))
            : std::format("{} of {} of links cached in RAM ({}), the rest read from disk", gb(info.cached_bytes),
                          gb(info.list_bytes), info.hot_set_profiled ? "profiled hot set" : "file order");
    const std::optional<wg::BuildReport>& built = state.prepare.built;
    const std::string timing =
        built ? std::format("Built from the dumps in {:.1f} s (peak memory {}), opened in {:.2f} s", built->seconds,
                            gb(built->peak_footprint), info.open_seconds)
              : std::format("Opened in {:.2f} s", info.open_seconds);
    return vbox({create_text(std::format("Wikipedia ({}wiki {}) ready", state.selected.wiki.language_code,
                                         state.selected.wiki.date),
                             true),
                 separator(),
                 text(std::format("Articles: {:L} | links: {:L} | redirects: {:L}", info.nodes, info.edges,
                                  info.redirects)),
                 text(std::format("RAM budget {} -> {}", wg::budget_to_string(state.ram_budget), mode)),
                 hint(std::format("Always in RAM: {} (degrees, offsets, titles, search state) | process now uses {}",
                                  gb(info.fixed_bytes), gb(wg::process_footprint_bytes()))),
                 hint(timing)}) |
           border;
}
}  // namespace

void start_preparing_graph(UIState& state) {
    state.stage = UIStage::PrepareGraph;
    state.going_back = false;
    PrepareState& prepare = state.prepare;
    prepare.started = std::chrono::steady_clock::now();
    prepare.running = true;
    prepare.building = false;
    prepare.stop = std::stop_source{};
    prepare.error.clear();
    prepare.built.reset();
    spawn(state, [&state, dumps = state.selected.wiki.dump_files(), stop = prepare.stop.get_token()] {
        prepare_graph(state, dumps, stop);
    });
}

Component create_progress_ui(UIState& state) {
    return Renderer(Container::Vertical({}), [&state] {
        if (!state.prepare.error.empty())
            return vbox({create_text("Could not prepare the graph", true, Color::Red), separator(),
                         paragraph(state.prepare.error), separator(), hint("Esc: back to the wiki list")}) |
                   border;
        if (state.stage.load() == UIStage::PrepareGraph) return render_preparing(state);
        return render_graph_ready(state);
    });
}

// ------------------------------------------------------------------------------------------------ User input stage

namespace {
/// Tab in a title field: complete to the most linked-to article starting with the text. Returns false (so Tab moves
/// to the other field) when there is nothing to complete.
bool complete_title(const UIState& state, std::string& title, int& cursor) {
    if (state.graph == nullptr || !state.graph->is_open() || string_utils::trim(title).empty()) return false;
    const auto ids = state.graph->complete(title, 1);
    if (ids.empty()) return false;
    std::string full = state.graph->display_title(ids[0]);
    if (full == string_utils::trim(title)) return false;
    title = std::move(full);
    cursor = static_cast<int>(title.size());
    return true;
}

/// A single-line input: by default FTXUI inputs are multi-line, so Enter would insert a newline into the title.
Component title_input(std::string& title, int& cursor, const char* placeholder, std::function<void()> on_enter) {
    InputOption option;
    option.multiline = false;
    option.cursor_position = &cursor;
    option.on_enter = std::move(on_enter);
    return Input(&title, placeholder, option);
}
}  // namespace

Component create_input_ui(UIState& state) {
    // Enter in the start field moves to the end field; Enter in the end field searches
    SearchState& search = state.search;
    auto input_end =
        title_input(search.end_title, search.end_cursor, "End page title", [&state] { handle_search_submit(state); });
    auto input_start = title_input(search.start_title, search.start_cursor, "Start page title",
                                   [input_end] { input_end->TakeFocus(); });

    auto start_field = CatchEvent(input_start, [&state](const Event& e) {
        return e == Event::Tab && complete_title(state, state.search.start_title, state.search.start_cursor);
    });
    auto end_field = CatchEvent(input_end, [&state](const Event& e) {
        return e == Event::Tab && complete_title(state, state.search.end_title, state.search.end_cursor);
    });
    auto container = Container::Vertical({start_field, end_field});

    // Suggestions for the focused field, recomputed only when its text changes
    struct Suggestions {
        std::string text;
        std::vector<std::string> titles;
    };
    auto cache = std::make_shared<Suggestions>();

    return Renderer(container, [=, &state] {
        SearchState& s = state.search;
        if (s.focus_start) {
            s.focus_start = false;
            input_start->TakeFocus();
        }
        const std::string& typed = input_start->Focused() ? s.start_title : s.end_title;
        if (typed != cache->text) {
            cache->text = typed;
            cache->titles.clear();
            if (state.graph != nullptr && state.graph->is_open() && !string_utils::trim(typed).empty())
                for (const uint32_t id : state.graph->complete(typed, 6))
                    cache->titles.push_back(state.graph->display_title(id));
        }
        Elements lines = {create_text("Enter two page titles:", true), separator(),
                          hbox({text("Start: "), input_start->Render()}), hbox({text("End:   "), input_end->Render()}),
                          separator()};
        if (!cache->titles.empty()) {
            lines.push_back(hbox({hint("Suggestions: "), paragraph(string_utils::join(cache->titles, "  ·  "))}));
            lines.push_back(separator());
        }
        lines.push_back(!s.error.empty()
                            ? create_text(s.error, false, Color::Red)
                            : create_text("Enter: next field, then search | Tab: complete the title, or switch field | "
                                          "Esc: back to the wiki list",
                                          false, Color::Yellow));
        return vbox(std::move(lines)) | border;
    });
}

// ------------------------------------------------------------------------------------------------ Search functionality

namespace {
std::string did_you_mean(const wg::GraphStore& graph, const std::string& typed) {
    const auto ids = graph.complete(typed, 3);
    if (ids.empty()) return "";
    auto titles = ids | std::views::transform([&](uint32_t id) { return graph.display_title(id); });
    return " Did you mean: " + string_utils::join(titles, ", ") + "?";
}

/// Background thread. Writes the result and error, then clears `running`.
void perform_search(UIState& state, uint32_t from, uint32_t to, std::stop_token stop) {
    SearchState& search = state.search;
    try {
        Throttle refresh;
        search.result = state.graph->shortest_paths(
            from, to, SearchState::kPathsListed,
            [&](const wg::SearchProgress& p) {
                search.progress = p;
                if (refresh.due()) post_ui_refresh();
            },
            std::move(stop));
        if (search.result.cancelled)
            search.error = "Search cancelled.";
        else if (search.result.distance < 0)
            search.error = "No path found between the given pages.";
    } catch (const std::exception& e) {
        // e.g. a read error from the SSD, or out of memory: report it instead of terminating with a broken terminal
        spdlog::error("Search failed: {}", e.what());
        search.result = wg::PathResult{};
        search.error = std::string("The search failed: ") + e.what();
    }
    search.running = false;
    post_ui_refresh();
}

/// "USA redirects to United States · ...", for the titles that were redirects.
std::string redirect_note(const wg::GraphStore& graph, const SearchState& search, const wg::TitleMatch& start,
                          const wg::TitleMatch& end) {
    std::string note;
    for (const auto& [typed, m] : {std::pair{&search.start_title, start}, std::pair{&search.end_title, end}}) {
        if (!m.via_redirect) continue;
        note += std::format("{}{} redirects to {}", note.empty() ? "" : " · ", string_utils::trim(*typed),
                            graph.display_title(m.node));
    }
    return note;
}
}  // namespace

void handle_search_submit(UIState& state) {
    SearchState& search = state.search;
    if (search.running || state.graph == nullptr) return;
    // Problems with the titles are shown under the inputs, so they can be corrected in place
    search.error.clear();
    if (string_utils::trim(search.start_title).empty()) {
        search.focus_start = true;
        return;
    }
    if (string_utils::trim(search.end_title).empty()) return;  // Enter in the start field already moved here
    const wg::GraphStore& graph = *state.graph;
    const auto start = graph.find(search.start_title);
    if (!start) {
        search.error = "Start page not found: '" + search.start_title + "'." + did_you_mean(graph, search.start_title);
        search.focus_start = true;
        return;
    }
    const auto end = graph.find(search.end_title);
    if (!end) {
        search.error = "End page not found: '" + search.end_title + "'." + did_you_mean(graph, search.end_title);
        return;
    }
    search.redirect_note = redirect_note(graph, search, *start, *end);
    const uint32_t from = start->node, to = end->node;
    spdlog::debug("Searching {} -> {} (nodes {} -> {})", search.start_title, search.end_title, from, to);
    search.clear_result();
    search.progress = wg::SearchProgress{};
    search.stop = std::stop_source{};
    search.running = true;
    state.stage = UIStage::ShowPaths;
    spawn(state, [&state, from, to, stop = search.stop.get_token()] { perform_search(state, from, to, stop); });
}

// ------------------------------------------------------------------------------------------------ Results display
// stage

namespace {
Element render_search_progress(const wg::SearchProgress& p) {
    return vbox({create_text("Searching...", true, Color::Yellow), separator(),
                 text(std::format("Levels: {} forward, {} backward | frontier {:L} | lists read {:L}", p.forward_depth,
                                  p.backward_depth, p.frontier, p.lists_expanded)),
                 separator(), hint("Esc: cancel the search")}) |
           border;
}

/// The listed paths, one per row, with the highlighted one kept in view.
Element render_path_list(const UIState& state) {
    const wg::PathResult& r = state.search.result;
    Elements rows;
    rows.reserve(r.paths.size());
    for (size_t i = 0; i < r.paths.size(); ++i) {
        auto titles = r.paths[i] | std::views::transform([&](uint32_t id) { return state.graph->display_title(id); });
        Element row = text(std::format("{:>4}. {}", i + 1, string_utils::join(titles, " -> ")));
        if (static_cast<int>(i) == state.search.result_cursor) row = row | inverted | focus;
        rows.push_back(std::move(row));
    }
    return vbox(std::move(rows)) | vscroll_indicator | frame | flex;
}

/// The results box, with the keys that apply below it.
Element results_box(Elements elements) {
    elements.push_back(separator());
    elements.push_back(hint("Up/Down, PgUp/PgDn, Home/End: scroll | any letter: new search | Esc: edit this search"));
    return vbox(std::move(elements)) | border;
}

Element render_results(const UIState& state) {
    const SearchState& search = state.search;
    const wg::PathResult& r = search.result;
    Elements elements = {
        hbox({create_text("Shortest paths", true), hint(std::format(" (in {:.2f} ms)", r.milliseconds))}), separator()};
    if (!search.error.empty()) {
        elements.push_back(paragraph(search.error) | color(Color::Red));
        return results_box(std::move(elements));
    }
    const std::string count =
        r.path_count == UINT64_MAX ? "more than 18 quintillion" : std::format("{:L}", r.path_count);
    if (!search.redirect_note.empty()) elements.push_back(hint(search.redirect_note));
    elements.push_back(text(std::format("{} click{}, {} shortest path{}", r.distance,
                                        plural(static_cast<uint64_t>(r.distance)), count, plural(r.path_count))));
    const std::string disk = r.blocks_read > 0 ? std::format(", {:L} blocks from disk", r.blocks_read) : "";
    elements.push_back(hint(std::format("Searched {} level{}, read {:L} link lists ({:L} links){}", r.rounds,
                                        plural(r.rounds), r.lists_expanded, r.edges_scanned, disk)));
    elements.push_back(separator());
    elements.push_back(render_path_list(state));
    if (r.path_count > r.paths.size())
        elements.push_back(hint(std::format("Listing the first {:L} of {} paths", r.paths.size(), count)));
    return results_box(std::move(elements));
}
}  // namespace

Component create_results_ui(UIState& state) {
    return Renderer(Container::Vertical({}), [&state] {
        if (state.search.running) return render_search_progress(state.search.progress.load());
        return render_results(state);
    });
}

// ------------------------------------------------------------------------------------------------ Event handling

namespace {
/// Arrow keys, Page Up/Down, Home/End and the mouse wheel move the highlighted path.
bool scroll_results(const Event& event, SearchState& search) {
    const int last = std::max(0, static_cast<int>(search.result.paths.size()) - 1);
    auto move = [&](int delta) {
        search.result_cursor = std::clamp(search.result_cursor + delta, 0, last);
        return true;
    };
    if (event == Event::ArrowDown) return move(1);
    if (event == Event::ArrowUp) return move(-1);
    if (event == Event::PageDown) return move(kPageRows);
    if (event == Event::PageUp) return move(-kPageRows);
    if (event == Event::Home) return move(-last);
    if (event == Event::End) return move(last);
    if (event.is_mouse()) {
        const auto button = Event(event).mouse().button;  // Event::mouse() is not const in FTXUI 6
        if (button == Mouse::WheelDown) return move(3);
        if (button == Mouse::WheelUp) return move(-3);
    }
    return false;
}

bool handle_results_key(const Event& event, UIState& state) {
    SearchState& search = state.search;
    if (search.running) {
        if (event == Event::Escape) search.stop.request_stop();
        return true;  // ignore other keys until the search ends
    }
    if (event.is_mouse()) return scroll_results(event, search);
    if (scroll_results(event, search)) return true;
    if (event == Event::Escape) {  // back to the input, keeping the titles to edit
        search.clear_result();
        state.stage = UIStage::UserInput;
        return true;
    }
    if (event.is_character()) {  // a new search, starting with the letter typed
        search.new_search(event.character());
        state.stage = UIStage::UserInput;
        return true;
    }
    return false;
}

/// Stop the current stage's work and return to the wiki list once it has stopped.
void go_back(UIState& state, UIStage stage) {
    state.going_back = true;
    if (stage == UIStage::Download) state.download.stop.request_stop();
    if (stage == UIStage::PrepareGraph) state.prepare.stop.request_stop();
    finish_going_back_if_idle(state);
}
}  // namespace

bool handle_key_events(const Event& event, UIState& state) {
    const UIStage stage = state.stage.load();
    if (stage == UIStage::WikiSelection) {
        if (event != Event::Escape) return false;
        App::Active()->Exit();
        return true;
    }
    if (stage == UIStage::ShowPaths) return handle_results_key(event, state);

    // On the download error screen any key returns to the list; elsewhere (download, preparation, input) Esc does
    const bool download_failed = stage == UIStage::Download && !state.download.error.empty();
    if (event == Event::Escape || (download_failed && event.is_character())) {
        go_back(state, stage);
        return true;
    }
    return false;
}

}  // namespace wg::app
