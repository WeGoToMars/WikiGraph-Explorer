#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Graph/GraphFormat.h"
#include "Graph/Platform.h"

namespace wg {

/// A title the user typed, resolved to an article.
struct TitleMatch {
    uint32_t node = 0;
    bool via_redirect = false;  ///< the title was a redirect (e.g. "USA" for United States)
};

/// Article titles (titles.bin, in RAM) and redirect titles (redirects.bin, on disk behind a sparse index).
///
/// Titles are in database form: underscores for spaces, as in the dumps. Lookups accept what a user types: spaces
/// or underscores, and the first letter in either case (most wikis capitalise the first letter of every title).
class TitleIndex {
   public:
    void open(const std::filesystem::path& dir, const GraphMeta& meta);

    /// Title of a node in database form.
    [[nodiscard]] std::string_view title(uint32_t node) const {
        return {bytes_.data() + offsets_[node], offsets_[node + 1] - offsets_[node]};
    }

    /// The article a typed title names, directly or through a redirect.
    [[nodiscard]] std::optional<TitleMatch> find(std::string_view typed) const;

    /// Articles whose title starts with what the user typed, the exact match first, then by `rank` (higher
    /// first). Redirect titles are not included.
    [[nodiscard]] std::vector<uint32_t> complete(std::string_view typed_prefix, size_t limit,
                                                 std::span<const uint32_t> rank) const;

    [[nodiscard]] uint64_t redirect_count() const { return redirect_count_; }

   private:
    [[nodiscard]] std::optional<uint32_t> find_article(std::string_view title) const;
    [[nodiscard]] std::optional<uint32_t> find_redirect(std::string_view title) const;
    [[nodiscard]] std::string_view index_key(size_t entry) const {
        return {index_keys_.data() + index_key_offsets_[entry],
                index_key_offsets_[entry + 1] - index_key_offsets_[entry]};
    }

    // titles.bin: every node's title in one string, and node ids sorted by title for prefix search
    std::vector<uint32_t> offsets_;  // n + 1
    std::string bytes_;
    std::vector<uint32_t> by_title_;

    // redirects.idx (every kRedirectStride-th entry of redirects.bin) in RAM; redirects.bin read on lookup
    RandomAccessFile redirects_;
    std::vector<uint64_t> index_entry_offsets_;  // where each indexed entry starts in redirects.bin
    std::vector<uint32_t> index_key_offsets_;    // the indexed titles in index_keys_, one more than the entries
    std::string index_keys_;
    uint64_t redirect_count_ = 0;
};

}  // namespace wg
