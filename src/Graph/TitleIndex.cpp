#include "Graph/TitleIndex.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Graph/GraphFormat.h"
#include "Graph/Platform.h"
#include "Utils/StringUtils.h"

namespace wg {
namespace {

/// A value stored at `p`, which may be unaligned (file records are packed).
template <class T>
T load(const char* p) {
    T v{};
    std::memcpy(&v, p, sizeof v);
    return v;
}

/// What the user typed, in database form: leading whitespace (and trailing, with `trim_end`) removed, spaces made
/// underscores.
std::string to_database_form(std::string_view typed, bool trim_end) {
    std::string s(trim_end ? string_utils::trim(typed) : string_utils::trim_start(typed));
    std::ranges::replace(s, ' ', '_');
    return s;
}

/// The titles `key` can stand for: with its first letter capitalised first (the usual form), then as typed.
std::vector<std::string> spellings(std::string key) {
    std::vector<std::string> out;
    if (!key.empty() && key[0] >= 'a' && key[0] <= 'z') {
        std::string capitalised = key;
        capitalised[0] = static_cast<char>(capitalised[0] - 'a' + 'A');
        out.push_back(std::move(capitalised));
    }
    if (!key.empty()) out.push_back(std::move(key));
    return out;
}

}  // namespace

void TitleIndex::open(const std::filesystem::path& dir, const GraphMeta& meta) {
    const uint64_t n = meta.nodes;
    {
        RandomAccessFile f;
        f.open(dir / graph_file::kTitles, false);
        uint64_t at = 0;
        offsets_ = read_array<uint32_t>(f, n + 1, at);
        bytes_.resize(meta.title_bytes);
        if (meta.title_bytes != 0) f.read_exact(bytes_.data(), meta.title_bytes, at);
        at += meta.title_bytes;
        by_title_ = read_array<uint32_t>(f, n, at);
    }
    // Graphs built without a redirect dump have no redirect files
    if (!std::filesystem::exists(dir / graph_file::kRedirectIndex)) return;
    RandomAccessFile f;
    f.open(dir / graph_file::kRedirectIndex, false);
    std::string buf(f.size(), '\0');
    f.read_exact(buf.data(), buf.size(), 0);
    redirect_count_ = load<uint64_t>(buf.data());
    for (size_t p = sizeof(uint64_t); p + kRedirectIndexEntryHeader <= buf.size();) {
        const auto entry_offset = load<uint64_t>(buf.data() + p);
        const auto len = load<uint16_t>(buf.data() + p + sizeof(uint64_t));
        p += kRedirectIndexEntryHeader;
        index_entry_offsets_.push_back(entry_offset);
        index_key_offsets_.push_back(static_cast<uint32_t>(index_keys_.size()));
        index_keys_.append(buf.data() + p, len);
        p += len;
    }
    index_key_offsets_.push_back(static_cast<uint32_t>(index_keys_.size()));
    redirects_.open(dir / graph_file::kRedirects, false);
}

std::optional<uint32_t> TitleIndex::find_article(std::string_view t) const {
    const auto it = std::ranges::lower_bound(by_title_, t, {}, [&](uint32_t id) { return title(id); });
    if (it != by_title_.end() && title(*it) == t) return *it;
    return std::nullopt;
}

std::optional<uint32_t> TitleIndex::find_redirect(std::string_view t) const {
    if (index_entry_offsets_.empty()) return std::nullopt;
    // The stride of redirects.bin that can hold t starts at the last index entry whose title is <= t
    const auto entries = std::views::iota(size_t{0}, index_entry_offsets_.size());
    const auto after = std::ranges::upper_bound(entries, t, {}, [&](size_t i) { return index_key(i); });
    if (after == entries.begin()) return std::nullopt;
    const size_t i = *std::ranges::prev(after);
    const uint64_t a = index_entry_offsets_[i];
    const uint64_t b = i + 1 < index_entry_offsets_.size() ? index_entry_offsets_[i + 1] : redirects_.size();
    std::string stride(b - a, '\0');
    redirects_.read_exact(stride.data(), stride.size(), a);
    // Entries: { u16 length, title, u32 target node }
    for (size_t p = 0; p + sizeof(uint16_t) <= stride.size();) {
        const auto len = load<uint16_t>(stride.data() + p);
        const std::string_view key(stride.data() + p + sizeof(uint16_t), len);
        if (key == t) return load<uint32_t>(stride.data() + p + sizeof(uint16_t) + len);
        p += len + kRedirectEntryOverhead;
    }
    return std::nullopt;
}

std::optional<TitleMatch> TitleIndex::find(std::string_view typed) const {
    const auto candidates = spellings(to_database_form(typed, true));
    for (const std::string_view t : candidates)
        if (const auto node = find_article(t)) return TitleMatch{.node = *node, .via_redirect = false};
    for (const std::string_view t : candidates)
        if (const auto node = find_redirect(t)) return TitleMatch{.node = *node, .via_redirect = true};
    return std::nullopt;
}

std::vector<uint32_t> TitleIndex::complete(std::string_view typed_prefix, size_t limit,
                                           std::span<const uint32_t> rank) const {
    std::vector<uint32_t> out;
    if (limit == 0) return out;
    // Keep a trailing space: "New " should offer New_York but not Newton
    const auto prefixes = spellings(to_database_form(typed_prefix, false));

    // Collect a bounded number of prefix matches (by_title_ is sorted bytewise), then rank them
    constexpr size_t kMaxScanned = 20000;
    std::vector<uint32_t> found;
    std::optional<uint32_t> exact;
    for (const std::string_view prefix : prefixes) {
        auto it = std::ranges::lower_bound(by_title_, prefix, {}, [&](uint32_t id) { return title(id); });
        for (size_t scanned = 0; it != by_title_.end() && scanned < kMaxScanned; ++it, ++scanned) {
            const std::string_view t = title(*it);
            if (!t.starts_with(prefix)) break;
            if (t.size() == prefix.size()) {
                if (!exact) exact = *it;
            } else {
                found.push_back(*it);
            }
        }
    }
    const size_t want = std::min(found.size(), exact ? limit - 1 : limit);
    std::ranges::partial_sort(found, found.begin() + static_cast<std::ptrdiff_t>(want), std::greater{},
                              [&](uint32_t u) { return rank[u]; });
    if (exact) out.push_back(*exact);
    out.insert(out.end(), found.begin(), found.begin() + static_cast<std::ptrdiff_t>(want));
    return out;
}

}  // namespace wg
