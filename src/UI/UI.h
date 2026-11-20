#pragma once

#include <ftxui/component/component_base.hpp>

#include "UIBase.h"

namespace ftxui {
struct Event;
}

namespace wg::app {

// Actions (all on the UI thread)
/// Download the selected wiki (state.selected.prefix), then build and open its graph.
void start_download(UIState& state);
/// Build the graph of state.selected.wiki if needed, then open it under the RAM budget.
void start_preparing_graph(UIState& state);
/// Run the path search for the entered titles.
void handle_search_submit(UIState& state);
/// Return to the wiki list once no download, build or search is running (no-op unless going back).
void finish_going_back_if_idle(UIState& state);

// Component creation functions
ftxui::Component create_download_ui(UIState& state);
ftxui::Component create_input_ui(UIState& state);
ftxui::Component create_results_ui(UIState& state);
ftxui::Component create_progress_ui(UIState& state);
ftxui::Component create_main_ui(ftxui::Component& wiki_select_ui, ftxui::Component& download_ui,
                                ftxui::Component& input_ui, ftxui::Component& results_ui, ftxui::Component& progress_ui,
                                UIState& state);

/// Keys that apply across stages (Esc, result scrolling). Returns true if the event was handled.
bool handle_key_events(const ftxui::Event& event, UIState& state);

/// Run the UI until the user quits. Background work is cancelled and joined before this returns.
void run_ui(UIState& state);

}  // namespace wg::app
