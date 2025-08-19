#pragma once

#include <memory>

#include "DataLoaderBase.h"
#include "PageLoader.h"
#include "Utils/Hashmap.h"

/**
 * @brief Loads linktarget table mapping IDs to page indices.
 */
class LinkTargetLoader : public DataLoaderBase {
   private:
    std::unique_ptr<Hashmap<uint64_t, uint32_t>> linktarget_map_;

    // Diagnostics
    size_t total_linktargets_parsed_ = 0;
    size_t linktargets_mapped_ = 0;
    size_t title_not_found_in_pages_ = 0;

   public:
    /** @brief Construct an empty linktarget loader. */
    LinkTargetLoader();

    /** @brief Parse an INSERT line into (lt_id, title) pairs. */
    static std::vector<std::pair<uint64_t, std::string>> parse_line(const std::string& line);
    /** @brief Map linktarget IDs to page indices using the page loader. */
    void insert_linktargets(const std::vector<std::pair<uint64_t, std::string>>& linktargets,
                            const PageLoader& page_loader);

    // Load link targets from SQL file
    /** @brief Load and build the linktarget map from the SQL dump. */
    void load_linktarget_table(const WikiFile& file, const PageLoader& page_loader,
                               const ProgressCallback& progress_callback, std::chrono::milliseconds refresh_rate);

    // Find page index by link target ID
    /** @brief Lookup page index by linktarget ID. */
    bool find_page_index_by_linktarget_id(uint64_t lt_id, uint32_t& index) const;

    const Hashmap<uint64_t, uint32_t>* get_linktarget_map() const {
        return linktarget_map_.get();
    }

    // Memory management - destroy map when no longer needed
    /** @brief Free the linktarget map to reclaim memory. */
    void destroy_linktarget_map();

    // Check if map is available
    bool has_linktarget_map() const {
        return linktarget_map_ != nullptr;
    }

    // Get count
    size_t get_linktarget_count() const {
        return linktarget_map_ ? linktarget_map_->size() : 0;
    }
};
