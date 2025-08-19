#pragma once

#include <ftxui/component/component_base.hpp>
#include <functional>

using ftxui::Component;

#include "UIBase.h"

// Timing functions
/**
 * @brief Sum durations of load stages for display.
 */
std::chrono::milliseconds total_load_duration(UIState& state);

// Utility functions
/** @brief Kick off the BFS path search using current input. */
void perform_search(UIState& state);

/** @brief Download a single file and update progress. */
void download(UIState& state, WikiFileType type, std::string url);
/** @brief Download all needed files on a background thread. */
void download_in_background(UIState& state, DownloadURLs urls);

// Component creation functions
Component create_download_ui(UIState& state, std::function<void()> on_start_loading = nullptr);
Component create_input_ui(UIState& state);
Component create_results_ui(UIState& state);
Component create_progress_ui(UIState& state);
Component create_main_ui(Component& wiki_select_ui, Component& download_ui, Component& input_ui, Component& results_ui,
                         Component& progress_ui, UIState& state);

// Event handling functions
/** @brief Handle search submission from the input UI. */
void handle_search_submit(UIState& state);
/** @brief Translate library-specific key events into actions. */
bool handle_key_events(void* event, UIState& state);

// UI entry point
/** @brief Run the full UI flow including fetching stats. */
void run_ui(UIState& state, std::function<void()> on_wiki_selected = nullptr);
/** @brief Run the UI flow with pre-fetched stats. */
void run_ui_with_stats(UIState& state, std::vector<WikiEntry>& stats);
