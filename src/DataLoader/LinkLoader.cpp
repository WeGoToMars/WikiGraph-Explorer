#include "LinkLoader.h"

#include <chrono>

#include "FileReader/SQLParserUtils.h"
#include "spdlog/spdlog.h"

std::vector<std::pair<uint32_t, uint64_t>> LinkLoader::parse_line(const std::string& line) {
    auto tuples = extract_tuples(line);
    std::vector<std::pair<uint32_t, uint64_t>> links;
    links.reserve(tuples.size());

    for (const auto& tuple : tuples) {
        // https://www.mediawiki.org/wiki/Manual:Pagelinks_table
        SQLTupleParser parser(tuple);
        uint32_t page_from_id = 0;
        uint32_t page_from_namespace = 0;
        uint64_t link_target_id = 0;

        if (!parser.next_int(page_from_id)) continue;
        if (!parser.next_int(page_from_namespace)) continue;
        if (page_from_namespace != 0) continue;  // Skip non-article namespaces
        if (!parser.next_int(link_target_id)) continue;

        links.emplace_back(page_from_id, link_target_id);
    }

    return links;
}

void LinkLoader::insert_links(const std::vector<std::pair<uint32_t, uint64_t>>& links, const PageLoader& page_loader,
                              const LinkTargetLoader& linktarget_loader) {
    total_links_parsed_ += links.size();

    for (const auto& [page_from_id, link_target_id] : links) {
        uint32_t page_from_index = 0;
        uint32_t page_to_index = 0;
        const bool from_ok = page_loader.find_page_index_by_id(page_from_id, page_from_index);
        const bool to_ok = linktarget_loader.find_page_index_by_linktarget_id(link_target_id, page_to_index);

        if (from_ok && to_ok) {
            links_.emplace_back(page_from_index, page_to_index);
            links_inserted_++;
        } else {
            if (!from_ok) {
                page_from_id_miss_++;
            }
            if (!to_ok) {
                link_target_id_miss_++;
            }
        }
    }
}

void LinkLoader::load_pagelinks_table(const WikiFile& file, const PageLoader& page_loader,
                                      const LinkTargetLoader& linktarget_loader,
                                      const ProgressCallback& progress_callback,
                                      std::chrono::milliseconds refresh_rate) {
    init_reader(file);
    auto& reader = *this->reader_;

    auto start_time = std::chrono::steady_clock::now();
    auto last_time = start_time;

    parse_insert_lines(
        reader, parse_line,
        [&](const auto& links) {
            insert_links(links, page_loader, linktarget_loader);
            update_progress(links_.size(), progress_callback, reader, start_time, last_time, refresh_rate);
        },
        [&](const auto& first_links) {
            uint64_t num_links = estimated_number_of_items(file.data_path, first_links.size());
            links_.reserve(num_links);
            insert_links(first_links, page_loader, linktarget_loader);
        });

    update_progress(links_.size(), progress_callback, reader, start_time, last_time, refresh_rate, true);

    spdlog::info("LinkLoader stats: parsed={}, inserted={}, misses(from_id)={}, misses(link_target_id)={}",
                 total_links_parsed_, links_inserted_, page_from_id_miss_, link_target_id_miss_);
}

void LinkLoader::destroy_links() {
    spdlog::debug("Destroying links vector to free memory");
    links_.clear();
    links_.shrink_to_fit();
}
