#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "UI/UIBase.h"

// Forward declarations
struct Page;
struct Link;

/**
 * @brief Graph of Wikipedia pages
 */
class PageGraph {
   private:
    std::vector<std::vector<uint32_t>> adjacency_list;
    std::vector<Page> pages_;  // Store pages for UI access
    uint32_t number_of_links = 0;

    static std::unique_ptr<PageGraph> instance;
    static std::mutex mtx;

    struct BFSResult {
        std::vector<std::vector<uint32_t>> parents;
        uint32_t dist;
    };

    /**
     * @brief Run BFS and track parent layers for all shortest paths.
     */
    [[nodiscard]] BFSResult bfs_with_parents(UIState& state, uint32_t start_index, uint32_t end_index) const;

   public:
    /**
     * @brief Construct the graph from pages and links.
     */
    PageGraph(UIState& state, std::vector<Page>&& pages, std::vector<Link>&& links);  // constructor

    /** @brief Access the singleton graph instance. */
    static PageGraph& get();
    /** @brief Initialize the singleton with data and update UI progress while building. */
    static void init(UIState& state, std::vector<Page> pages, std::vector<Link> links);

    ~PageGraph();

    [[nodiscard]] uint32_t get_number_of_pages() const {
        return static_cast<uint32_t>(this->adjacency_list.size());
    }
    [[nodiscard]] uint32_t get_number_of_links() const {
        return this->number_of_links;
    }
    [[nodiscard]] const std::vector<std::vector<uint32_t>>& get_adjacency_list() const {
        return this->adjacency_list;
    }
    [[nodiscard]] const std::vector<Page>& get_pages() const {
        return this->pages_;
    }

    /**
     * @brief Compute all shortest paths between two nodes.
     */
    std::vector<std::vector<uint32_t>> all_shortest_paths(UIState& state, uint32_t start_index,
                                                          uint32_t end_index) const;
};
