#include "DataLoaderManager.h"

#include <spdlog/spdlog.h>

#include "PageGraph/PageGraph.h"

DataLoaderManager::DataLoaderManager()
    : page_loader_(std::make_unique<PageLoader>()),
      linktarget_loader_(std::make_unique<LinkTargetLoader>()),
      link_loader_(std::make_unique<LinkLoader>()) {}

void DataLoaderManager::cleanup_after_linktarget_load() {
    // Keep the page title lookup map alive for UI searches
}

void DataLoaderManager::cleanup_after_link_load() {
    page_loader_->destroy_id_lookup();
    linktarget_loader_->destroy_linktarget_map();
}

void DataLoaderManager::cleanup_after_graph_build() {
    link_loader_->destroy_links();
}

// Define the loader function that will be called when a wiki is selected
void start_loader_thread(UIState& state, std::unique_ptr<DataLoaderManager>& data_manager) {
    std::thread loader([&] {
        std::string wiki_prefix = state.selected_wiki_prefix;
        std::string date = state.selected_wiki_date;
        spdlog::debug("Loading wiki: {} {}", wiki_prefix, date);

        // Page loading stage
        auto start_time = std::chrono::steady_clock::now();
        spdlog::debug("Loading page table...");
        spdlog::debug("file: {}", state.selected_wiki.page.data_path.string());
        data_manager->get_page_loader().load_page_table(
            state.selected_wiki.page,
            [&](size_t count, uint32_t speed, ReadProgress progress) {
                state.page_count = count;
                state.page_speed = speed;
                state.page_progress = progress;
                post_ui_refresh();
            },
            UIState::refresh_rate);
        auto end_time = std::chrono::steady_clock::now();
        state.page_load_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        state.stage = UIStage::LoadLinkTargets;
        post_ui_refresh();

        // Link target loading stage
        start_time = std::chrono::steady_clock::now();
        spdlog::debug("Loading linktarget table...");
        spdlog::debug("file: {}", state.selected_wiki.linktarget.data_path.string());
        data_manager->get_linktarget_loader().load_linktarget_table(
            state.selected_wiki.linktarget, data_manager->get_page_loader(),
            [&](size_t count, uint32_t speed, ReadProgress progress) {
                state.linktarget_count = count;
                state.linktarget_speed = speed;
                state.linktarget_progress = progress;
                post_ui_refresh();
            },
            UIState::refresh_rate);
        end_time = std::chrono::steady_clock::now();
        state.linktarget_load_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Clean up after linktarget loading
        data_manager->cleanup_after_linktarget_load();

        state.stage = UIStage::LoadLinks;
        post_ui_refresh();

        // Link loading stage
        start_time = std::chrono::steady_clock::now();
        spdlog::debug("Loading pagelinks table...");
        spdlog::debug("file: {}", state.selected_wiki.pagelinks.data_path.string());
        data_manager->get_link_loader().load_pagelinks_table(
            state.selected_wiki.pagelinks, data_manager->get_page_loader(), data_manager->get_linktarget_loader(),
            [&](size_t count, uint32_t speed, ReadProgress progress) {
                state.link_count = count;
                state.link_speed = speed;
                state.link_progress = progress;
                post_ui_refresh();
            },
            UIState::refresh_rate);
        end_time = std::chrono::steady_clock::now();
        state.link_load_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Clean up after link loading
        data_manager->cleanup_after_link_load();

        state.stage = UIStage::BuildingGraph;
        post_ui_refresh();

        start_time = std::chrono::steady_clock::now();
        // Build the graph with the loaded data
        PageGraph::init(data_manager->move_pages(), data_manager->move_links());

        // Clean up after graph construction
        data_manager->cleanup_after_graph_build();

        end_time = std::chrono::steady_clock::now();
        state.graph_build_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        state.stage = UIStage::UserInput;
        post_ui_refresh();
    });
    loader.detach();
}
