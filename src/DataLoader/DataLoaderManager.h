#pragma once
#include <memory>

#include "LinkLoader.h"
#include "LinkTargetLoader.h"
#include "PageLoader.h"

// Manager class that coordinates all data loaders and handles memory management
/**
 * @brief Coordinates page, linktarget, and link loading and manages memory lifecycles.
 */
class DataLoaderManager {
   private:
    std::unique_ptr<PageLoader> page_loader_;
    std::unique_ptr<LinkTargetLoader> linktarget_loader_;
    std::unique_ptr<LinkLoader> link_loader_;

   public:
    /**
     * @brief Construct an empty manager and allocate loader instances.
     */
    DataLoaderManager();

    // Accessors
    /** @brief Access the page loader. */
    PageLoader& get_page_loader() {
        return *page_loader_;
    }
    /** @brief Const access to the page loader. */
    const PageLoader& get_page_loader() const {
        return *page_loader_;
    }
    /** @brief Access the linktarget loader. */
    LinkTargetLoader& get_linktarget_loader() {
        return *linktarget_loader_;
    }
    /** @brief Const access to the linktarget loader. */
    const LinkTargetLoader& get_linktarget_loader() const {
        return *linktarget_loader_;
    }
    /** @brief Access the link loader. */
    LinkLoader& get_link_loader() {
        return *link_loader_;
    }
    /** @brief Const access to the link loader. */
    const LinkLoader& get_link_loader() const {
        return *link_loader_;
    }

    // Memory management - called at appropriate times to free unused data
    /** @brief Free memory no longer needed after linktarget load (title lookup). */
    void cleanup_after_linktarget_load();
    /** @brief Free memory no longer needed after link load (ID lookup and linktarget map). */
    void cleanup_after_link_load();
    /** @brief Free links after graph has been built. */
    void cleanup_after_graph_build();

    // Get data for graph construction (returns by move)
    /** @brief Move out the loaded pages. */
    std::vector<Page> move_pages() {
        return page_loader_->move_pages();
    }
    /** @brief Move out the loaded links. */
    std::vector<Link> move_links() {
        return link_loader_->move_links();
    }
};

/**
 * @brief Start the asynchronous data loading thread and update `UIState`.
 * @param state UI state to receive progress updates
 * @param data_manager Manager containing loader instances
 */
void start_loader_thread(UIState& state, std::unique_ptr<DataLoaderManager>& data_manager);
