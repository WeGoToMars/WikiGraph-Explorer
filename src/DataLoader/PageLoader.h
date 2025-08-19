#pragma once
#include <memory>
#include <string>
#include <vector>

#include "DataLoaderBase.h"
#include "UI/UIBase.h"
#include "Utils/Hashmap.h"

struct Page {
    std::string page_title;
    bool page_is_redirect;
};

/**
 * @brief Loads page metadata and redirects from the SQL dump.
 */
class PageLoader : public DataLoaderBase {
   private:
    std::vector<Page> pages_;

    std::unique_ptr<Hashmap<uint32_t, uint32_t>> page_id_to_index_;
    std::unique_ptr<Hashmap<std::string, uint32_t>> page_title_to_index_;

    std::unique_ptr<Hashmap<std::string, uint32_t>> redirects_;

    /**
     * @brief Insert a batch of parsed pages and update lookup maps.
     */
    void insert_pages(const std::vector<std::pair<uint32_t, Page>>& batch);

   public:
    PageLoader() = default;

    // Load pages from SQL file
    /**
     * @brief Load and parse the page table from a compressed SQL file.
     */
    void load_page_table(const WikiFile& file, const ProgressCallback& progress_callback,
                         std::chrono::milliseconds refresh_rate);

    /**
     * @brief Parse an INSERT line into page records keyed by page_id.
     */
    static std::vector<std::pair<uint32_t, Page>> parse_line(const std::string& line);

    // Accessors
    /** @brief Get a page by internal index. */
    [[nodiscard]] const Page& get_page(uint32_t index) const {
        return pages_[index];
    }
    /** @brief Move out the internal pages vector. */
    std::vector<Page> move_pages() {
        return std::move(pages_);
    }
    /** @brief Number of loaded pages. */
    [[nodiscard]] size_t get_page_count() const {
        return pages_.size();
    }

    // Find page index by ID or title
    /** @brief Find page index by Wikipedia page id. */
    bool find_page_index_by_id(uint32_t page_id, uint32_t& index) const;
    /** @brief Find page index by page title (after resolving redirects). */
    bool find_page_index_by_title(const std::string& title, uint32_t& index) const;

    // Memory management - destroy lookup maps when no longer needed
    /** @brief Free the page id lookup map to reclaim memory. */
    void destroy_id_lookup();
    /** @brief Free the page title lookup map to reclaim memory. */
    void destroy_title_lookup();

    // Check if lookups are available
    /** @brief Whether the page id lookup map is available. */
    bool has_id_lookup() const {
        return page_id_to_index_ != nullptr;
    }
    /** @brief Whether the page title lookup map is available. */
    bool has_title_lookup() const {
        return page_title_to_index_ != nullptr;
    }
};
