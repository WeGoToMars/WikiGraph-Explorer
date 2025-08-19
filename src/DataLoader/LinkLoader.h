#pragma once

#include <vector>

#include "DataLoaderBase.h"
#include "LinkTargetLoader.h"
#include "PageLoader.h"

// These are indexes into the pages vector, not Wikipedia page ids
struct Link {
    uint32_t page_from;
    uint32_t page_to;
};

/**
 * @brief Loads page-to-page links from the SQL dump.
 */
class LinkLoader : public DataLoaderBase {
   private:
    std::vector<Link> links_;

    // Diagnostics
    size_t total_links_parsed_ = 0;
    size_t links_inserted_ = 0;
    size_t page_from_id_miss_ = 0;
    size_t link_target_id_miss_ = 0;

   public:
    // Load page links from SQL file
    /** @brief Load and resolve page links using page and linktarget loaders. */
    void load_pagelinks_table(const WikiFile& file, const PageLoader& page_loader,
                              const LinkTargetLoader& linktarget_loader, const ProgressCallback& progress_callback,
                              std::chrono::milliseconds refresh_rate);

    /** @brief Parse an INSERT line into (page_from_id, linktarget_id) pairs. */
    static std::vector<std::pair<uint32_t, uint64_t>> parse_line(const std::string& line);

    /** @brief Insert resolved links into the adjacency list backing store. */
    void insert_links(const std::vector<std::pair<uint32_t, uint64_t>>& links, const PageLoader& page_loader,
                      const LinkTargetLoader& linktarget_loader);

    // Accessors
    std::vector<Link> move_links() {
        return std::move(links_);
    }
    [[nodiscard]] size_t get_link_count() const {
        return links_.size();
    }

    // Destroy links when no longer needed
    /** @brief Free stored links to reclaim memory after graph construction. */
    void destroy_links();
};
