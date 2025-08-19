#include "LinkTargetLoader.h"

#include <chrono>

#include "FileReader/SQLParserUtils.h"
#include "spdlog/spdlog.h"

LinkTargetLoader::LinkTargetLoader() : linktarget_map_(std::make_unique<Hashmap<uint64_t, uint32_t>>()) {}

std::vector<std::pair<uint64_t, std::string>> LinkTargetLoader::parse_line(const std::string& line) {
    auto tuples = extract_tuples(line);
    std::vector<std::pair<uint64_t, std::string>> linktargets;
    linktargets.reserve(tuples.size());

    for (const auto& tuple : tuples) {
        SQLTupleParser parser(tuple);
        uint64_t lt_id = 0;
        uint32_t lt_namespace = 0;
        std::string lt_title;

        if (!parser.next_int(lt_id)) continue;
        if (!parser.next_int(lt_namespace)) continue;
        if (lt_namespace != 0) continue;  // Skip non-article namespaces
        if (!parser.next_string(lt_title)) continue;

        linktargets.emplace_back(lt_id, lt_title);
    }

    return linktargets;
}

void LinkTargetLoader::insert_linktargets(const std::vector<std::pair<uint64_t, std::string>>& linktargets,
                                          const PageLoader& page_loader) {
    total_linktargets_parsed_ += linktargets.size();
    for (const auto& [lt_id, lt_title] : linktargets) {
        uint32_t page_index = 0;
        if (page_loader.find_page_index_by_title(lt_title, page_index)) {
            linktarget_map_->emplace(lt_id, page_index);
            linktargets_mapped_++;
        } else {
            title_not_found_in_pages_++;
        }
    }
}

void LinkTargetLoader::load_linktarget_table(const WikiFile& file, const PageLoader& page_loader,
                                             const ProgressCallback& progress_callback,
                                             std::chrono::milliseconds refresh_rate) {
    init_reader(file);
    auto& reader = *this->reader_;

    auto start_time = std::chrono::steady_clock::now();
    auto last_time = start_time;

    std::string line;

    linktarget_map_->reserve(page_loader.get_page_count());

    parse_insert_lines(reader, parse_line, [&](const auto& result) {
        insert_linktargets(result, page_loader);
        update_progress(linktarget_map_->size(), progress_callback, reader, start_time, last_time, refresh_rate);
    });

    update_progress(linktarget_map_->size(), progress_callback, reader, start_time, last_time, refresh_rate, true);

    spdlog::info("LinkTargetLoader stats: parsed={}, mapped={}, title_misses={}", total_linktargets_parsed_,
                 linktargets_mapped_, title_not_found_in_pages_);
}

bool LinkTargetLoader::find_page_index_by_linktarget_id(uint64_t lt_id, uint32_t& index) const {
    if (!linktarget_map_) return false;

    auto iter = linktarget_map_->find(lt_id);
    if (iter != linktarget_map_->end()) {
        index = iter->second;
        return true;
    }
    return false;
}

void LinkTargetLoader::destroy_linktarget_map() {
    if (linktarget_map_) {
        spdlog::debug("Destroying linktarget map to free memory");
        linktarget_map_.reset();
    }
}
