#include "PageLoader.h"

#include <chrono>
#include <string_view>

#include "FileReader/SQLParserUtils.h"
#include "spdlog/spdlog.h"

std::vector<std::pair<uint32_t, Page>> PageLoader::parse_line(const std::string& line) {
    auto tuples = extract_tuples(line);
    std::vector<std::pair<uint32_t, Page>> pages;
    pages.reserve(tuples.size());

    for (const auto& tuple : tuples) {
        // https://www.mediawiki.org/wiki/Manual:Page_table
        SQLTupleParser parser(tuple);
        uint32_t page_id = 0;
        int32_t page_namespace = 0;
        std::string page_title;
        bool page_is_redirect = false;

        if (!parser.next_int(page_id)) continue;
        if (!parser.next_int(page_namespace)) continue;
        if (page_namespace != 0) continue;  // Only load pages from main namespace (articles)
        if (!parser.next_string(page_title)) continue;
        if (!parser.next_bool(page_is_redirect)) continue;

        pages.emplace_back(page_id, Page{.page_title = page_title, .page_is_redirect = page_is_redirect});
    }

    return pages;
}

void PageLoader::insert_pages(const std::vector<std::pair<uint32_t, Page>>& batch) {
    const size_t start_index = pages_.size();
    for (size_t i = 0; i < batch.size(); i++) {
        const auto& [page_id, page] = batch[i];
        const auto index = static_cast<uint32_t>(start_index + i);
        pages_.emplace_back(page.page_title, page.page_is_redirect);
        page_id_to_index_->emplace(page_id, index);
        page_title_to_index_->emplace(page.page_title, index);
    }
}

void PageLoader::load_page_table(const WikiFile& file, const ProgressCallback& progress_callback,
                                 std::chrono::milliseconds refresh_rate) {
    if (!reader_) {
        init_reader(file);
    }
    if (!page_id_to_index_) {
        page_id_to_index_ = std::make_unique<Hashmap<uint32_t, uint32_t>>();
    }
    if (!page_title_to_index_) {
        page_title_to_index_ = std::make_unique<Hashmap<std::string, uint32_t>>();
    }
    auto& reader = this->reader_;

    auto start_time = std::chrono::steady_clock::now();
    auto last_time = start_time;

    parse_insert_lines(
        *reader, parse_line,
        [&](const auto& result) {
            insert_pages(result);
            update_progress(pages_.size(), progress_callback, *reader, start_time, last_time, refresh_rate);
        },
        [&](const auto& first_result) {
            uint64_t num_pages = estimated_number_of_items(file.data_path, first_result.size());

            pages_.reserve(num_pages);
            page_id_to_index_->reserve(num_pages);
            page_title_to_index_->reserve(num_pages);
            insert_pages(first_result);
        });

    update_progress(pages_.size(), progress_callback, *reader, start_time, last_time, refresh_rate, true);

    // The page vector will be used through the lifetime of the program,
    // so it's better to shrink it to optimize memory usage.
    pages_.shrink_to_fit();
}

bool PageLoader::find_page_index_by_id(uint32_t page_id, uint32_t& index) const {
    if (!page_id_to_index_) return false;

    auto iter = page_id_to_index_->find(page_id);
    if (iter != page_id_to_index_->end()) {
        index = iter->second;
        return true;
    }
    return false;
}

bool PageLoader::find_page_index_by_title(const std::string& title, uint32_t& index) const {
    if (!page_title_to_index_) return false;

    auto iter = page_title_to_index_->find(title);
    if (iter != page_title_to_index_->end()) {
        index = iter->second;
        return true;
    }
    return false;
}

void PageLoader::destroy_id_lookup() {
    if (page_id_to_index_) {
        spdlog::debug("Destroying page ID lookup map to free memory");
        page_id_to_index_.reset();
    }
}

void PageLoader::destroy_title_lookup() {
    if (page_title_to_index_) {
        spdlog::debug("Destroying page title lookup map to free memory");
        page_title_to_index_.reset();
    }
}
