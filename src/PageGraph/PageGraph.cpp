#include "PageGraph.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <queue>
#include <stack>
#include <stdexcept>
#include <vector>

#include "DataLoader/LinkLoader.h"
#include "DataLoader/PageLoader.h"
#include "UI/UIBase.h"
#include "spdlog/spdlog.h"

std::unique_ptr<PageGraph> PageGraph::instance = nullptr;
std::mutex PageGraph::mtx;

// Constuct page graph from pages and links
PageGraph::PageGraph(UIState& state, std::vector<Page>&& pages, std::vector<Link>&& links)
    : pages_(std::move(pages)) {  // Move pages for UI access
    // Pages = nodes, Links = edges
    this->adjacency_list.resize(pages_.size());

    // Take ownership of links, vector is automatically destroyed when it goes out of scope
    std::vector<Link> links_ = std::move(links);

    // Count number of outgoing links for each page
    std::vector<uint32_t> out_links_count(pages_.size());
    for (const auto& link : links_) {
        out_links_count[link.page_from]++;
    }

    // Reserve space for outgoing links for each page
    // This is a performance optimization to avoid reallocations
    for (uint32_t i = 0; i < pages_.size(); i++) {
        adjacency_list[i].reserve(out_links_count[i]);
    }

    // Initialize graph build progress
    state.graph_build_progress = {
        .processed_links = 0, .total_links = static_cast<uint64_t>(links_.size()), .edges_speed = 0};

    // Construct adjacency list with periodic UI updates
    auto start_time = std::chrono::steady_clock::now();
    auto last_update_time = start_time;
    for (const auto& link : links_) {
        this->adjacency_list[link.page_from].emplace_back(link.page_to);
        this->number_of_links++;

        // throttle UI updates purely by time using the UI state's refresh rate
        auto now = std::chrono::steady_clock::now();
        if (now - last_update_time >= UIState::refresh_rate) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            uint32_t speed = elapsed_ms > 0
                                 ? static_cast<uint32_t>((static_cast<double>(this->number_of_links) * 1000.0) /
                                                         static_cast<double>(elapsed_ms))
                                 : 0u;
            state.graph_build_progress = {.processed_links = this->number_of_links,
                                          .total_links = static_cast<uint64_t>(links_.size()),
                                          .edges_speed = speed};
            post_ui_refresh();
            last_update_time = now;
        }
    }

    // Final update
    auto end_time = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    uint32_t final_speed = total_ms > 0 ? static_cast<uint32_t>((static_cast<double>(this->number_of_links) * 1000.0) /
                                                                static_cast<double>(total_ms))
                                        : 0u;
    state.graph_build_progress = {.processed_links = this->number_of_links,
                                  .total_links = static_cast<uint64_t>(links_.size()),
                                  .edges_speed = final_speed};
    post_ui_refresh();

    spdlog::debug("PageGraph constructed with {} pages and {} links", pages_.size(), this->number_of_links);
    // links vector is automatically destroyed when it goes out of scope
}

PageGraph::~PageGraph() {
    this->adjacency_list.clear();
}

PageGraph& PageGraph::get() {
    std::lock_guard<std::mutex> lock(mtx);

    if (!instance) {
        throw std::runtime_error("PageGraph not initialized. Call PageGraph::init() first.");
    }

    return *instance;
}

void PageGraph::init(UIState& state, std::vector<Page> pages, std::vector<Link> links) {
    std::lock_guard<std::mutex> lock(mtx);

    if (!instance) {
        instance = std::make_unique<PageGraph>(state, std::move(pages), std::move(links));
    }
}

PageGraph::BFSResult PageGraph::bfs_with_parents(UIState& state, uint32_t start_index, uint32_t end_index) const {
    const auto& adj = this->adjacency_list;

    std::vector<uint32_t> dist(adj.size(), UINT32_MAX);
    std::vector<std::vector<uint32_t>> parents(adj.size());
    std::queue<uint32_t> queue;

    queue.push(start_index);
    dist[start_index] = 0;

    // Track BFS progress
    uint32_t current_layer = 0;
    uint32_t layer_size = 0;

    uint32_t layer_explored_count = 0;
    uint32_t total_explored_count = 0;
    // Throttle UI updates to avoid excessive refreshes
    auto last_update_time = std::chrono::steady_clock::now();

    while (!queue.empty()) {
        uint32_t current_node = queue.front();
        queue.pop();

        if (dist[current_node] > current_layer) {  // We are entering a new layer
            if (dist[end_index] != UINT32_MAX) {   // We have found the end node, stop BFS
                break;
            }

            current_layer = dist[current_node];
            // +1 to account for the node we just popped as part of the new layer
            layer_size = static_cast<uint32_t>(queue.size()) + 1;
            total_explored_count += layer_explored_count;
            layer_explored_count = 0;

            state.bfs_progress = {.current_layer = current_layer,
                                  .total_explored_nodes = total_explored_count,
                                  .layer_size = layer_size,
                                  .layer_explored_count = layer_explored_count};
            spdlog::debug("BFS progress: layer {} ({} nodes), {} nodes explored", current_layer, layer_size,
                          total_explored_count);

            // Trigger UI refresh to show progress
            post_ui_refresh();
            last_update_time = std::chrono::steady_clock::now();
        }

        for (uint32_t neighbor : adj[current_node]) {
            if (dist[neighbor] == UINT32_MAX) {
                dist[neighbor] = dist[current_node] + 1;
                parents[neighbor].emplace_back(current_node);
                queue.push(neighbor);
            } else if (dist[neighbor] == dist[current_node] + 1) {
                parents[neighbor].emplace_back(current_node);
            }
        }

        // Finished processing one node in the current layer
        layer_explored_count++;

        // Periodically refresh after processing nodes
        auto now = std::chrono::steady_clock::now();
        if (now - last_update_time >= UIState::refresh_rate) {
            state.bfs_progress = {.current_layer = current_layer,
                                  .layer_size = layer_size,
                                  .layer_explored_count = layer_explored_count,
                                  .total_explored_nodes = total_explored_count + layer_explored_count};
            post_ui_refresh();
            last_update_time = now;
        }
    }

    // Final update
    state.bfs_progress = {.current_layer = current_layer,
                          .layer_size = layer_size,
                          .layer_explored_count = layer_explored_count,
                          .total_explored_nodes = total_explored_count + layer_explored_count};
    post_ui_refresh();

    return {.parents = std::move(parents), .dist = dist[end_index]};
}

std::vector<std::vector<uint32_t>> PageGraph::all_shortest_paths(UIState& state, uint32_t start_index,
                                                                 uint32_t end_index) const {
    const auto& adj = this->adjacency_list;

    std::vector<std::vector<uint32_t>> paths;
    if (start_index >= adj.size() || end_index >= adj.size()) {
        spdlog::error("all_shortest_paths start_index {} or end_index {} is out of bounds (graph size: {})",
                      start_index, end_index, adj.size());
        return paths;
    }

    auto bfs_result = bfs_with_parents(state, start_index, end_index);
    spdlog::debug("BFS result: dist={}, parents={}", bfs_result.dist, bfs_result.parents.size());
    const auto& parents = bfs_result.parents;
    const auto& dist = bfs_result.dist;

    // Backtrack all paths from end_index to start_index iteratively using DFS
    if (dist != UINT32_MAX) {
        spdlog::debug("Shortest path distance is {}. Backtracking to find all paths.", dist);
        std::stack<std::vector<uint32_t>> path_stack;
        path_stack.push({end_index});

        while (!path_stack.empty()) {
            std::vector<uint32_t> current_path = std::move(path_stack.top());
            path_stack.pop();

            uint32_t current_node = current_path.back();

            if (current_node == start_index) {
                std::ranges::reverse(current_path);
                paths.push_back(std::move(current_path));

                continue;
            }

            for (uint32_t parent_node : parents[current_node]) {
                std::vector<uint32_t> new_path = current_path;
                new_path.push_back(parent_node);
                path_stack.push(std::move(new_path));
            }
        }
    }

    return paths;
}
