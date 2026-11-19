#include "Graph/GraphStore.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "Graph/Budget.h"
#include "Graph/GraphFormat.h"
#include "Graph/Log.h"
#include "Graph/NeighbourLists.h"
#include "Graph/Parallel.h"
#include "Graph/PathSearch.h"
#include "Graph/Platform.h"
#include "Graph/TitleIndex.h"

namespace wg {
namespace {

GraphMeta read_meta(const std::filesystem::path& dir) {
    RandomAccessFile f;
    f.open(dir / graph_file::kMeta, false);
    GraphMeta m{};
    f.read_exact(&m, sizeof m, 0);
    if (m.magic != kGraphMagic || m.version != kGraphVersion)
        throw std::runtime_error("not a graph directory of this version: " + dir.string());
    return m;
}

GraphFootprint footprint(const GraphMeta& m, const std::filesystem::path& dir) {
    const uint64_t n = m.nodes;
    std::error_code ec;
    const uint64_t redirect_index = std::filesystem::file_size(dir / graph_file::kRedirectIndex, ec);
    const uint64_t degrees_and_offsets = n * 2 * sizeof(uint32_t) + (n + 1) * 2 * sizeof(uint64_t);
    const uint64_t titles = (n + 1) * sizeof(uint32_t) + m.title_bytes + n * sizeof(uint32_t);
    return {.fixed_bytes = degrees_and_offsets + n * PathSearch::kBytesPerNode + titles + (ec ? 0 : redirect_index),
            .list_bytes = (blocks_for(m.list_bytes[0]) + blocks_for(m.list_bytes[1])) * kBlockSize};
}

struct ResidentBlocks {
    std::vector<uint64_t> keys;
    bool profiled = false;  ///< chosen from the profile in hot.bin
};

/// The `count` blocks most worth keeping in RAM: the most-read in the build's profile (hot.bin), or without one,
/// the first blocks of both files, which hold the hubs.
ResidentBlocks choose_resident_blocks(const std::filesystem::path& dir, const NeighbourLists& lists, uint64_t count) {
    ResidentBlocks chosen;
    if (std::ifstream hot(dir / graph_file::kHotBlocks, std::ios::binary); hot) {
        chosen.profiled = true;
        chosen.keys.resize(count);
        hot.read(reinterpret_cast<char*>(chosen.keys.data()), static_cast<std::streamsize>(count * sizeof(uint64_t)));
        chosen.keys.resize(static_cast<size_t>(hot.gcount()) / sizeof(uint64_t));
    }
    if (chosen.keys.empty()) {
        const uint64_t longest = std::max(lists.block_count(Dir::Fwd), lists.block_count(Dir::Rev));
        for (uint64_t b = 0; chosen.keys.size() < count && b < longest; ++b)
            for (const Dir d : kDirs)
                if (chosen.keys.size() < count && b < lists.block_count(d)) chosen.keys.push_back(block_key(d, b));
    }
    return chosen;
}

}  // namespace

struct GraphStore::Impl {
    NeighbourLists lists;
    TitleIndex titles;
    std::unique_ptr<PathSearch> search;  // created last in open()
};

GraphStore::GraphStore() = default;
GraphStore::~GraphStore() = default;

GraphStore::Impl& GraphStore::impl() {
    if (!impl_) throw std::logic_error("no graph is open");
    return *impl_;
}

const GraphStore::Impl& GraphStore::impl() const {
    if (!impl_) throw std::logic_error("no graph is open");
    return *impl_;
}

GraphFootprint GraphStore::footprint_of(const std::filesystem::path& dir) { return footprint(read_meta(dir), dir); }

void GraphStore::open(const std::filesystem::path& dir, uint64_t budget_bytes) {
    std::lock_guard lk(search_mutex_);
    const auto t0 = std::chrono::steady_clock::now();
    const GraphMeta meta = read_meta(dir);
    auto im = std::make_unique<Impl>();
    im->lists.open(dir, meta);
    im->titles.open(dir, meta);

    // Whatever the budget leaves after the fixed part holds list blocks
    GraphInfo info;
    const GraphFootprint need = footprint(meta, dir);
    const uint64_t budget = resolve_budget(budget_bytes, need.fixed_bytes + need.list_bytes);
    const uint64_t blocks = budget > need.fixed_bytes ? (budget - need.fixed_bytes) / kBlockSize : 0;
    if (blocks >= im->lists.block_count(Dir::Fwd) + im->lists.block_count(Dir::Rev)) {
        im->lists.make_all_resident();
        info.all_in_ram = true;
    } else if (blocks > 0) {
        ResidentBlocks chosen = choose_resident_blocks(dir, im->lists, blocks);
        info.hot_set_profiled = chosen.profiled;
        im->lists.make_resident(std::move(chosen.keys));
    }

    im->search = std::make_unique<PathSearch>(meta.nodes);  // after the lists: see the PathSearch constructor

    info.meta = meta;
    info.nodes = meta.nodes;
    info.edges = meta.edges;
    info.redirects = im->titles.redirect_count();
    info.list_bytes = need.list_bytes;
    info.fixed_bytes = need.fixed_bytes;
    info.budget_bytes = budget;
    info.cached_bytes = im->lists.resident_bytes();
    info.open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    impl_ = std::move(im);
    info_ = info;
    log_info(
        "Opened graph {}: {} nodes, {} edges, budget {} -> {} ({:.0f} MB of lists cached), fixed {:.0f} MB, {:.2f} s",
        dir.string(), info_.nodes, info_.edges, budget_to_string(budget_bytes),
        info_.all_in_ram ? "all lists in RAM" : "SSD + hot set", static_cast<double>(info_.cached_bytes) / kBytesPerMB,
        static_cast<double>(info_.fixed_bytes) / kBytesPerMB, info_.open_seconds);
}

void GraphStore::close() {
    std::lock_guard lk(search_mutex_);
    impl_.reset();
    info_ = GraphInfo{};
}

std::optional<TitleMatch> GraphStore::find(std::string_view user_title) const { return impl().titles.find(user_title); }

std::vector<uint32_t> GraphStore::complete(std::string_view typed_prefix, size_t limit) const {
    const Impl& im = impl();
    return im.titles.complete(typed_prefix, limit, im.lists.degrees(Dir::Rev));  // most linked-to first
}

std::string_view GraphStore::title(uint32_t node) const { return impl().titles.title(node); }

std::string GraphStore::display_title(uint32_t node) const {
    std::string s(title(node));
    std::ranges::replace(s, '_', ' ');
    return s;
}

PathResult GraphStore::shortest_paths(uint32_t from, uint32_t to, size_t max_paths,
                                      const std::function<void(const SearchProgress&)>& progress,
                                      std::stop_token stop) {
    std::lock_guard lk(search_mutex_);
    Impl& im = impl();
    return im.search->run(im.lists, from, to, max_paths, progress, std::move(stop));
}

std::span<const uint32_t> GraphStore::degrees(Dir direction) const { return impl().lists.degrees(direction); }

void GraphStore::export_csr(Dir d, std::span<uint32_t> offsets, std::span<uint32_t> targets) const {
    const Impl& im = impl();
    const std::span<const uint32_t> deg = im.lists.degrees(d);
    const auto n = static_cast<uint32_t>(deg.size());
    if (offsets.size() != n + size_t{1} || targets.size() != info_.edges)
        throw std::invalid_argument("export_csr: offsets need n + 1 entries and targets one per link");
    uint64_t total = 0;
    for (uint32_t u = 0; u < n; ++u) {
        offsets[u] = static_cast<uint32_t>(total);
        total += deg[u];
        if (total > UINT32_MAX) throw std::runtime_error("export_csr: too many links for 32-bit offsets");
    }
    offsets[n] = static_cast<uint32_t>(total);
    if (total != info_.edges)
        throw std::runtime_error(
            std::format("export_csr: degrees sum to {} links, the graph has {}", total, info_.edges));

    // Ranges of nodes with about equal list bytes; each is read with one positional read, then decoded
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned parts = threads * 4;
    const std::span<const uint64_t> list_offsets = im.lists.offsets(d);
    const uint64_t bytes = list_offsets[n];
    std::vector<uint32_t> cut(parts + 1, n);
    cut[0] = 0;
    for (unsigned i = 1; i < parts; ++i) {
        const auto it = std::lower_bound(list_offsets.begin(), list_offsets.begin() + n, bytes * i / parts);
        cut[i] = std::max(cut[i - 1], static_cast<uint32_t>(it - list_offsets.begin()));
    }
    parallel_for(parts, threads, [&](size_t i) {
        const uint32_t a = cut[i], b = cut[i + 1];
        if (a >= b) return;
        const uint64_t lo = list_offsets[a], hi = list_offsets[b];
        std::vector<uint8_t> buf(hi - lo);
        if (hi > lo) im.lists.file(d).read_exact(buf.data(), hi - lo, lo);
        for (uint32_t u = a; u < b; ++u)
            decode_list(buf.data() + (list_offsets[u] - lo), targets.subspan(offsets[u], deg[u]));
    });
}

std::vector<std::pair<uint32_t, uint32_t>> GraphStore::random_pairs(size_t count, uint64_t seed) const {
    const NeighbourLists& lists = impl().lists;
    const auto candidates = std::views::iota(0u, static_cast<uint32_t>(lists.degrees(Dir::Fwd).size())) |
                            std::views::filter([&](uint32_t u) {
                                return lists.degree(Dir::Fwd, u) != 0 && lists.degree(Dir::Rev, u) != 0;
                            }) |
                            std::ranges::to<std::vector>();
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (candidates.empty()) return out;
    std::mt19937_64 rng(seed);
    for (size_t i = 0; i < count; ++i) {
        // Two statements: argument evaluation order differs between compilers, and the pairs must match everywhere
        const uint32_t from = candidates[rng() % candidates.size()];
        const uint32_t to = candidates[rng() % candidates.size()];
        out.emplace_back(from, to);
    }
    return out;
}

std::vector<uint64_t> GraphStore::profile_blocks(size_t queries, uint64_t seed) {
    const auto pairs = random_pairs(queries, seed);
    {
        std::lock_guard lk(search_mutex_);
        impl().lists.start_profile();
    }
    for (const auto& [s, t] : pairs) (void)shortest_paths(s, t, 0);
    std::lock_guard lk(search_mutex_);
    return impl().lists.stop_profile();
}

}  // namespace wg
