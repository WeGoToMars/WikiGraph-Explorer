#include "UI.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DataLoader/PageLoader.h"
#include "FetchWikiData/DownloadWikiDump.h"
#include "PageGraph/PageGraph.h"
#include "UIBase.h"
#include "Utils/PathUtils.h"
#include "WikiSelectUI.h"
#include "spdlog/spdlog.h"

using namespace ftxui;

//=============================================================================
// UTILITY FUNCTIONS
//=============================================================================

namespace {
constexpr double kBytesPerMB = 1024 * 1024;

/// @brief Trim leading and trailing whitespace from a string.
/// @param string The input string to trim.
/// @return A new string with leading and trailing whitespace removed.
std::string trim(const std::string& string) {
    auto start = string.begin();
    while (start != string.end() && std::isspace(*start)) start++;
    auto end = string.end();
    do {
        end--;
    } while (std::distance(start, end) > 0 && (std::isspace(*end) != 0));
    return {start, end + 1};
}

// Create formatted text with optional color
Element create_text(const std::string& content, bool bold = false, Color color = Color::Default) {
    auto elem = text(content);
    if (bold) elem = elem | ftxui::bold;
    if (color != Color::Default) elem = elem | ftxui::color(color);
    return elem;
}

// Create text with timing information
Element create_timed_text(const std::string& content, int64_t milliseconds) {
    return hbox({text(content), text(" (in " + std::to_string(milliseconds) + " ms)") | color(Color::GrayDark)});
}

// Create progress display with count and speed
Elements create_progress_display(const std::string& label, size_t count, double speed, const std::string& unit = "",
                                 float progress_ratio = -1) {
    Elements elements = {text(label + ": " + std::format("{:L}", count)),
                         text("Speed: " + std::format("{:L} {}/sec", static_cast<int>(speed), unit))};

    if (progress_ratio >= 0) {
        elements.push_back(
            hbox({text(std::format("Progress: {:.2f}%", progress_ratio * 100)), text(" "), gauge(progress_ratio)}));
    }

    return elements;
}

// Create step header
Element create_step_header(const std::string& step, const std::string& description) {
    return text("[" + step + "] " + description) | bold;
}
}  // namespace

//=============================================================================
// CORE UTILITY FUNCTIONS
//=============================================================================

std::chrono::milliseconds total_load_duration(UIState& state) {
    return state.page_load_duration + state.linktarget_load_duration + state.link_load_duration +
           state.graph_build_duration;
}

//=============================================================================
// MAIN UI ENTRY POINT
//=============================================================================

void run_ui(UIState& state, std::function<void()> on_wiki_selected) {
    // Use fullscreen for all stages for consistent behavior
    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    // Fetch wiki statistics
    std::vector<WikiEntry> stats = fetch_wiki_stats();

    if (stats.empty()) {
        state.offline_mode = true;
    }

    // Create components
    Component wiki_select_ui = create_wiki_select_ui(state, stats, std::move(on_wiki_selected));
    Component download_ui = create_download_ui(state, on_wiki_selected);
    Component progress_ui = create_progress_ui(state);
    Component input_ui = create_input_ui(state);
    Component results_ui = create_results_ui(state);
    Component main_ui = create_main_ui(wiki_select_ui, download_ui, input_ui, results_ui, progress_ui, state);

    // Set up additional event handling for global events (like ESC to exit)
    auto main_with_global_events = CatchEvent(main_ui, [&](Event event) { return handle_key_events(&event, state); });

    // Run the UI loop
    screen.Loop(main_with_global_events);

    // Clean up - shared_ptr components are automatically cleaned up
}

//=============================================================================
// WIKI SELECTION STAGE
//=============================================================================

Component create_main_ui(Component& wiki_select_ui, Component& download_ui, Component& input_ui, Component& results_ui,
                         Component& progress_ui, UIState& state) {
    // Create a renderer that switches between components based on stage
    auto main_ui = Renderer([&] {
        switch (state.stage.load()) {
            case UIStage::WikiSelection:
                // Center the wiki selector nicely within fullscreen
                return center(wiki_select_ui->Render()) | flex;

            case UIStage::Download: {
                return download_ui->Render();
            }

            case UIStage::LoadPages:
            case UIStage::LoadLinkTargets:
            case UIStage::LoadLinks:
            case UIStage::BuildingGraph:
                return progress_ui->Render();

            case UIStage::UserInput:
                return vbox({progress_ui->Render(), input_ui->Render()});

            case UIStage::ShowPaths:
                return vbox({progress_ui->Render(), results_ui->Render()});

            case UIStage::Done:
                return text("Done") | border;
        }
    });

    // Wrap with event handling that delegates to the appropriate component
    auto main_with_events = CatchEvent(main_ui, [&](const Event& event) {
        if (state.stage == UIStage::WikiSelection) {
            return wiki_select_ui->OnEvent(event);
        } else if (state.stage == UIStage::Download) {
            return download_ui->OnEvent(event);
        } else if (state.stage == UIStage::UserInput) {
            return input_ui->OnEvent(event);
        } else if (state.stage == UIStage::ShowPaths) {
            return results_ui->OnEvent(event);
        }
        return false;
    });

    return main_with_events;
}

//=============================================================================
// DOWNLOAD STAGE
//=============================================================================

void download(UIState& state, WikiFileType type, std::string url) {
    struct DownloadConfig {
        std::atomic<UIState::DownloadProgress>* progress_ptr;
        std::atomic<bool>* complete_ptr;
        std::string filename_suffix;
    };

    DownloadConfig config{};
    switch (type) {
        case WikiFileType::Page:
            config = {.progress_ptr = &state.page_download_progress,
                      .complete_ptr = &state.page_download_complete,
                      .filename_suffix = "-page.sql.gz"};
            break;
        case WikiFileType::PageLinks:
            config = {.progress_ptr = &state.pagelinks_download_progress,
                      .complete_ptr = &state.pagelinks_download_complete,
                      .filename_suffix = "-pagelinks.sql.gz"};
            break;
        case WikiFileType::LinkTarget:
            config = {.progress_ptr = &state.linktarget_download_progress,
                      .complete_ptr = &state.linktarget_download_complete,
                      .filename_suffix = "-linktarget.sql.gz"};
            break;
    }
    std::filesystem::path full_path =
        PathUtils::get_resource_dir("data") /
        (state.selected_wiki_prefix + "wiki-" + state.selected_wiki_date + config.filename_suffix);
    download_file(std::move(url), full_path.string(), *config.progress_ptr, UIState::refresh_rate);
    config.complete_ptr->store(true);

    UIState::DownloadProgress dp = config.progress_ptr->load();
    // Ensure the progress bar reaches 100% on completion
    config.progress_ptr->store({.dlnow = dp.dltotal, .dltotal = dp.dltotal, .dlspeed = 0});
    post_ui_refresh();

    // Don't refresh UI from download threads - let the main UI handle updates
}

void download_in_background(UIState& state, DownloadURLs urls) {
    if (urls.page.empty() || urls.pagelinks.empty() || urls.linktarget.empty()) {
        state.download_error_message = "Could not find download URLs for the selected wiki.";
        post_ui_refresh();
        return;
    }

    std::thread download_pages([&, urls] { download(state, WikiFileType::Page, urls.page); });
    std::thread download_pagelinks([&, urls] { download(state, WikiFileType::PageLinks, urls.pagelinks); });
    std::thread download_linktarget([&, urls] { download(state, WikiFileType::LinkTarget, urls.linktarget); });

    download_pages.detach();
    download_pagelinks.detach();
    download_linktarget.detach();
}

static Element render_download_progress(std::atomic<UIState::DownloadProgress>& dp) {
    const auto [dlnow, dltotal, dlspeed] = dp.load();
    return hbox({gauge(static_cast<float>(dlnow) / static_cast<float>(dltotal)) | flex, text(" "),
                 text(std::format("{:5.2f} MB / {:5.2f} MB", static_cast<double>(dlnow) / kBytesPerMB,
                                  static_cast<double>(dltotal) / kBytesPerMB)),
                 text(" | "), text(std::format("{:5.2f} MB/s", static_cast<double>(dlspeed) / kBytesPerMB))});
}

static Element render_download_ui(UIState& state) {
    if (!state.download_error_message.empty()) {
        return vbox({create_text("Download Error", true), separator(),
                     create_text(state.download_error_message, false, Color::Red), separator(),
                     text("Press any key to return to wiki selection.")}) |
               border;
    }

    auto page_speed = static_cast<double>(state.page_download_progress.load().dlspeed);
    auto pagelinks_speed = static_cast<double>(state.pagelinks_download_progress.load().dlspeed);
    auto linktarget_speed = static_cast<double>(state.linktarget_download_progress.load().dlspeed);
    double total_speed = page_speed + pagelinks_speed + linktarget_speed;

    Elements elements;
    std::string url =
        std::format("https://dumps.wikimedia.org/{}wiki/{}/", state.selected_wiki_prefix, state.selected_wiki_date);
    elements.push_back(hbox({create_text(std::format("Downloading {}wiki from ", state.selected_wiki_prefix), true),
                             create_text(url, true, Color::GrayDark) | hyperlink(url),
                             create_text(std::format(" at {:.2f} MB/s", total_speed / kBytesPerMB), true)}));
    elements.push_back(separator());

    // Page file
    elements.push_back(text("Page file:"));
    elements.push_back(render_download_progress(state.page_download_progress));

    // Page links file
    elements.push_back(text("Page links file:"));
    elements.push_back(render_download_progress(state.pagelinks_download_progress));

    // Link target file
    elements.push_back(text("Link target file:"));
    elements.push_back(render_download_progress(state.linktarget_download_progress));

    if (state.page_download_complete && state.pagelinks_download_complete && state.linktarget_download_complete) {
        state.stage = UIStage::LoadPages;
    }

    return vbox(std::move(elements)) | border;
}

Component create_download_ui(UIState& state, std::function<void()> on_start_loading) {
    auto dummy_container = Container::Vertical({});

    // Create a renderer that will start the download when the component is first rendered
    auto download_ui = Renderer(dummy_container, [&] {
        // Start download only once when this component is first rendered
        static bool download_started = false;
        if (!download_started && state.stage == UIStage::Download) {
            download_started = true;

            std::string wiki_prefix = state.selected_wiki_prefix;
            spdlog::debug("Creating download UI for {}", wiki_prefix);

            if (!wiki_prefix.empty()) {
                DownloadURLs urls = get_urls_from_rss(wiki_prefix);
                state.selected_wiki_date = urls.date;
                download_in_background(state, urls);

                // Start a timer to periodically refresh the UI during downloads
                std::thread refresh_timer([&] {
                    while (state.stage == UIStage::Download) {
                        std::this_thread::sleep_for(UIState::refresh_rate);
                        if (state.stage == UIStage::Download) {
                            post_ui_refresh();
                        }
                    }
                });
                refresh_timer.detach();
            }
        }

        // If all downloads have completed, populate selected_wiki and kick off loading
        if (state.page_download_complete && state.pagelinks_download_complete && state.linktarget_download_complete) {
            // Populate selected_wiki paths for the freshly downloaded files
            if (!state.selected_wiki_prefix.empty() && !state.selected_wiki_date.empty()) {
                const auto base = PathUtils::get_resource_dir("data");
                const auto prefix = state.selected_wiki_prefix;
                const auto date = state.selected_wiki_date;
                auto make_file = [&](const char* suffix, WikiFileType type) {
                    WikiFile f{};
                    f.exists = true;
                    f.lang_code = prefix;
                    f.date = date;
                    f.file_type = type;
                    f.data_path = base / (prefix + std::string("wiki-") + date + suffix);
                    if (std::filesystem::exists(f.data_path)) {
                        f.file_size = std::filesystem::file_size(f.data_path);
                    } else {
                        f.exists = false;
                    }
                    return f;
                };

                state.selected_wiki.page = make_file("-page.sql.gz", WikiFileType::Page);
                state.selected_wiki.pagelinks = make_file("-pagelinks.sql.gz", WikiFileType::PageLinks);
                state.selected_wiki.linktarget = make_file("-linktarget.sql.gz", WikiFileType::LinkTarget);
                state.selected_wiki.language_code = prefix;
                state.selected_wiki.date = date;
                state.selected_wiki.size_on_disk = state.selected_wiki.page.file_size +
                                                   state.selected_wiki.pagelinks.file_size +
                                                   state.selected_wiki.linktarget.file_size;
            }

            if (on_start_loading) {
                // Advance stage and start loader thread via callback
                state.stage = UIStage::LoadPages;
                post_ui_refresh();
                on_start_loading();
            }
        }

        return render_download_ui(state);
    });

    return download_ui;
}

//=============================================================================
// LOADING STAGE
//=============================================================================

Component create_progress_ui(UIState& state) {
    auto dummy_container = Container::Vertical({});
    auto progress_ui = Renderer(dummy_container, [&] {
        Elements elements;

        if (state.stage.load() < UIStage::UserInput) {
            // Loading stages
            switch (state.stage.load()) {
                case UIStage::WikiSelection:
                    break;
                case UIStage::Download:
                    break;
                case UIStage::LoadPages: {
                    float progress_ratio = static_cast<float>(state.page_progress.load().current_bytes) /
                                           static_cast<float>(state.page_progress.load().total_bytes);
                    elements = {create_step_header("Step 1/4", "Loading Wikipedia pages..."), separator()};
                    auto progress_elems = create_progress_display("Loaded pages", state.page_count.load(),
                                                                  state.page_speed.load(), "pages", progress_ratio);
                    elements.insert(elements.end(), progress_elems.begin(), progress_elems.end());
                    elements.insert(elements.end(), {text(" "), create_text("Loading...", false, Color::Yellow)});
                    break;
                }
                case UIStage::LoadLinkTargets: {
                    float progress_ratio = static_cast<float>(state.linktarget_progress.load().current_bytes) /
                                           static_cast<float>(state.linktarget_progress.load().total_bytes);
                    elements = {create_step_header("Step 2/4", "Loading Wikipedia link targets..."), separator()};
                    auto progress_elems =
                        create_progress_display("Loaded link targets", state.linktarget_count.load(),
                                                state.linktarget_speed.load(), "targets", progress_ratio);
                    elements.insert(elements.end(), progress_elems.begin(), progress_elems.end());
                    elements.insert(elements.end(), {text(" "), create_text("Loading...", false, Color::Yellow)});
                    break;
                }
                case UIStage::LoadLinks: {
                    float progress_ratio = static_cast<float>(state.link_progress.load().current_bytes) /
                                           static_cast<float>(state.link_progress.load().total_bytes);
                    elements = {create_step_header("Step 3/4", "Loading Wikipedia links..."), separator()};
                    auto progress_elems = create_progress_display("Loaded links", state.link_count.load(),
                                                                  state.link_speed.load(), "links", progress_ratio);
                    elements.insert(elements.end(), progress_elems.begin(), progress_elems.end());
                    elements.insert(elements.end(), {text(" "), create_text("Loading...", false, Color::Yellow)});
                    break;
                }
                case UIStage::BuildingGraph: {
                    auto gb = state.graph_build_progress.load();
                    float progress_ratio =
                        gb.total_links > 0 ? static_cast<float>(gb.processed_links) / static_cast<float>(gb.total_links)
                                           : 0.0f;
                    elements = {create_step_header("Step 4/4", "Building graph..."),
                                separator(),
                                text("Loaded pages: " + std::format("{:L}", state.page_count.load())),
                                text("Loaded link targets: " + std::format("{:L}", state.linktarget_count.load())),
                                text("Loaded links: " + std::format("{:L}", state.link_count.load())),
                                text(" ")};
                    auto build_elems =
                        create_progress_display("Edges inserted", gb.processed_links,
                                                static_cast<double>(gb.edges_speed), "edges", progress_ratio);
                    elements.insert(elements.end(), build_elems.begin(), build_elems.end());
                    elements.push_back(text(" "));
                    elements.push_back(create_text("Building...", false, Color::Yellow));
                    break;
                }
                case UIStage::UserInput:
                    break;
                case UIStage::ShowPaths:
                    break;
                case UIStage::Done:
                    break;
            }
        } else {
            // Completed stage
            elements = {
                create_text(std::format("Wikipedia ({}wiki) loaded!", state.selected_wiki_prefix), true),
                separator(),
                create_timed_text("Loaded pages: " + std::format("{:L}", state.page_count.load()),
                                  state.page_load_duration.count()),
                create_timed_text("Loaded link targets: " + std::format("{:L}", state.linktarget_count.load()),
                                  state.linktarget_load_duration.count()),
                create_timed_text("Loaded links: " + std::format("{:L}", state.link_count.load()),
                                  state.link_load_duration.count()),
                hbox({text("Graph built in " + std::to_string(state.graph_build_duration.count()) + " ms =>"),
                      create_text(" Total " + std::to_string(total_load_duration(state).count()) + " ms", true)})};
        }
        return vbox(std::move(elements)) | border;
    });

    return progress_ui;
}

//=============================================================================
// USER INPUT STAGE
//=============================================================================

Component create_input_ui(UIState& state) {
    auto input_start = Input(&state.start_title, "Start page title");

    InputOption end_input_option;
    end_input_option.on_enter = [&] { handle_search_submit(state); };
    auto input_end = Input(&state.end_title, "End page title", end_input_option);

    auto container = Container::Vertical({input_start, input_end});
    auto input_ui = Renderer(container, [=, &state] {
        return vbox({create_text("Enter two page titles:", true), separator(),
                     hbox({text("Start: "), input_start->Render()}), hbox({text("End:   "), input_end->Render()}),
                     separator(),
                     !state.error_message.empty()
                         ? create_text(state.error_message, false, Color::Red)
                         : create_text("Press Enter in the End field to search.", false, Color::Yellow)}) |
               border;
    });

    return input_ui;
}

//=============================================================================
// SEARCH FUNCTIONALITY
//=============================================================================

void perform_search(UIState& state) {
    state.error_message.clear();
    state.found_paths.clear();
    state.is_searching = true;
    state.bfs_progress = {.current_layer = 0, .layer_size = 0, .layer_explored_count = 0, .total_explored_nodes = 0};

    // Look up indices
    const std::string start_page = trim(state.start_title);
    const std::string end_page = trim(state.end_title);

    // Get the PageGraph to access page data
    const PageGraph& graph = PageGraph::get();
    if (start_page.empty() || end_page.empty()) {
        state.error_message = "Please enter both start and end page titles.";
        return;
    }

    uint32_t start_idx = UINT32_MAX;
    uint32_t end_idx = UINT32_MAX;

    if (state.page_loader != nullptr && state.page_loader->has_title_lookup()) {
        if (!state.page_loader->find_page_index_by_title(start_page, start_idx)) {
            state.error_message = "Start page not found: '" + start_page + "'";
            return;
        }
        if (!state.page_loader->find_page_index_by_title(end_page, end_idx)) {
            state.error_message = "End page not found: '" + end_page + "'";
            return;
        }
    } else {
        state.error_message = "Hmm, page loader not initialized, please create an issue on GitHub if you see this.";
        return;
    }

    // Perform the search and record the time it took
    const auto start_time = std::chrono::steady_clock::now();
    spdlog::debug("Searching for {} -> {} (indices: {} -> {})", start_page, end_page, start_idx, end_idx);

    // Diagnostics: log out-degree of start node to verify outgoing edges
    const auto& adj = graph.get_adjacency_list();
    if (start_idx < adj.size()) {
        spdlog::debug("Start node '{}' (idx {}) out-degree: {}", start_page, start_idx, adj[start_idx].size());
    }
    state.found_paths = graph.all_shortest_paths(state, start_idx, end_idx);

    const auto end_time = std::chrono::steady_clock::now();
    state.search_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    if (state.found_paths.empty()) {
        state.error_message = "No path found between the given pages.";
    }
    state.is_searching = false;
    post_ui_refresh();
}

void handle_search_submit(UIState& state) {
    std::thread search_thread([&state] { perform_search(state); });
    search_thread.detach();
    state.stage = UIStage::ShowPaths;
    post_ui_refresh();
}

//=============================================================================
// RESULTS DISPLAY STAGE
//=============================================================================

Component create_results_ui(UIState& state) {
    auto dummy_container = Container::Vertical({});
    auto results_ui = Renderer(dummy_container, [&] {
        Elements elements = {hbox({create_text("All Shortest Paths:", true),
                                   text(" (in " + std::to_string(state.search_duration.count()) + " milliseconds)") |
                                       color(Color::GrayDark)}),
                             separator()};

        // Show BFS progress if search is in progress
        auto bfs_progress = state.bfs_progress.load();
        if (bfs_progress.current_layer > 0 || bfs_progress.total_explored_nodes > 0) {
            if (state.is_searching) {
                elements.push_back(create_text("BFS Search Progress:", true, Color::Yellow));
            } else {
                elements.push_back(create_text("BFS Search Complete:", true, Color::Green));
            }
            elements.push_back(text("Current layer: " + std::to_string(bfs_progress.current_layer)));
            elements.push_back(text("Nodes explored: " + std::format("{:L}", bfs_progress.total_explored_nodes)));
            elements.push_back(text("Current layer size: " + std::format("{:L}", bfs_progress.layer_size)));

            // Calculate progress percentage based on total graph size
            const auto& graph = PageGraph::get();
            uint32_t total_nodes = graph.get_number_of_pages();
            float layer_progress_ratio = bfs_progress.layer_size > 0
                                             ? static_cast<float>(bfs_progress.layer_explored_count) /
                                                   static_cast<float>(bfs_progress.layer_size)
                                             : 0.0f;
            float graph_progress_ratio =
                static_cast<float>(bfs_progress.total_explored_nodes) / static_cast<float>(total_nodes);
            if (layer_progress_ratio < 1.0f) {
                elements.push_back(hbox({text(std::format("Layer {} progress: {:.1f}%", bfs_progress.current_layer,
                                                          layer_progress_ratio * 100)),
                                         text(" "), gauge(layer_progress_ratio)}));
            } else {
                elements.push_back(text(std::format("Layer {} progress: 100.0%", bfs_progress.current_layer)));
            }
            elements.push_back(
                hbox({text(std::format("Total graph traversal progress: {:.1f}%", graph_progress_ratio * 100)),
                      text(" "), gauge(graph_progress_ratio)}));
            elements.push_back(separator());
        }

        if (!state.error_message.empty()) {
            elements.push_back(create_text(state.error_message, false, Color::Red));
        } else if (state.is_searching && state.found_paths.empty()) {
            elements.push_back(create_text("Searching...", false, Color::Yellow));
        } else if (state.found_paths.empty()) {
            elements.push_back(text("No paths found."));
        } else {
            elements.push_back(text("Number of paths: " + std::to_string(state.found_paths.size())));

            // Generate path strings
            const auto& pages = PageGraph::get().get_pages();  // This will need to be added to PageGraph
            for (const auto& path : state.found_paths) {
                std::string line;
                for (size_t i = 0; i < path.size(); ++i) {
                    line += pages[path[i]].page_title;
                    if (i + 1 < path.size()) line += " -> ";
                }
                elements.push_back(text(line));
            }
        }

        elements.insert(elements.end(), {separator(), text("Press any key to search again or ESC to exit.")});

        return vbox(std::move(elements)) | border;
    });

    return results_ui;
}

//=============================================================================
// EVENT HANDLING
//=============================================================================

bool handle_key_events(void* event_ptr, UIState& state) {
    auto& event = *static_cast<Event*>(event_ptr);

    if (state.stage == UIStage::ShowPaths) {
        if (event == Event::Escape) {
            ScreenInteractive::Active()->Exit();
            return true;
        } else if (event.is_character()) {
            state.start_title.clear();
            state.end_title.clear();
            state.error_message.clear();
            state.found_paths.clear();
            state.stage = UIStage::UserInput;
            post_ui_refresh();
            return true;
        }
    } else if (state.stage == UIStage::Download && !state.download_error_message.empty()) {
        // Handle key press when download error is shown
        if (event.is_character()) {
            state.download_error_message.clear();
            state.stage = UIStage::WikiSelection;
            post_ui_refresh();
            return true;
        }
    }
    return false;
}
