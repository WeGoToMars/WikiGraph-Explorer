#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Graph/Budget.h"
#include "Graph/GraphFormat.h"
#include "Graph/PathSearch.h"
#include "Graph/TitleIndex.h"

namespace wg {

struct GraphInfo {
    uint32_t nodes = 0;
    uint64_t edges = 0;
    uint64_t redirects = 0;
    uint64_t list_bytes = 0;    ///< both neighbour-list files
    uint64_t fixed_bytes = 0;   ///< always resident: degrees, offsets, titles, search state, redirect index
    uint64_t budget_bytes = 0;  ///< the budget that was applied
    uint64_t cached_bytes = 0;  ///< neighbour-list bytes held in RAM
    bool all_in_ram = false;    ///< every list is in RAM; searches do no I/O
    bool hot_set_profiled = false;
    double open_seconds = 0;
    GraphMeta meta{};
};

/// The RAM a graph needs: always, and to hold every neighbour list (in whole blocks).
struct GraphFootprint {
    uint64_t fixed_bytes = 0;
    uint64_t list_bytes = 0;
};

/// A built graph opened under a RAM budget.
///
/// Degrees, list offsets, titles and the search state are always in RAM. If the budget also covers every neighbour
/// list, they are loaded and searches never touch the disk. Otherwise the remaining budget holds the hottest 16 KB
/// blocks (from the profile in hot.bin) and everything else is read from the SSD, one batch per BFS level.
///
/// Everything except open(), close(), is_open(), info() and footprint_of() needs an open graph and throws
/// std::logic_error without one. Searches throw std::runtime_error on a read error. Searches are serialised; title
/// lookups may run alongside them.
class GraphStore {
   public:
    GraphStore();
    ~GraphStore();
    GraphStore(const GraphStore&) = delete;
    GraphStore& operator=(const GraphStore&) = delete;

    /// Open a graph directory written by build_graph(), replacing any graph already open. Throws on failure (the
    /// graph open before stays open).
    void open(const std::filesystem::path& dir, uint64_t budget_bytes);
    /// Release the graph and its memory. Waits for a running search to finish.
    void close();
    [[nodiscard]] bool is_open() const { return impl_ != nullptr; }

    [[nodiscard]] const GraphInfo& info() const { return info_; }

    /// Resolve a user-typed title (spaces or underscores, first letter in either case, or a redirect).
    [[nodiscard]] std::optional<TitleMatch> find(std::string_view user_title) const;
    /// Articles whose title starts with what the user typed (spaces or underscores, first letter in either
    /// case), most linked-to first; an exact match always comes first. Redirect titles are not included.
    [[nodiscard]] std::vector<uint32_t> complete(std::string_view typed_prefix, size_t limit) const;
    /// Title of a node in database form (underscores).
    [[nodiscard]] std::string_view title(uint32_t node) const;
    /// Title of a node for display (spaces).
    [[nodiscard]] std::string display_title(uint32_t node) const;

    /**
     * Shortest paths from `from` to `to` (bidirectional BFS that counts all shortest paths).
     * @param max_paths  how many paths to list; the count is always exact (or saturated)
     * @param progress   called after each BFS level, may be empty
     * @param stop       checked once per level; a stop request ends the search with `cancelled` set
     */
    PathResult shortest_paths(uint32_t from, uint32_t to, size_t max_paths,
                              const std::function<void(const SearchProgress&)>& progress = {},
                              std::stop_token stop = {});

    /// Out-degree (Dir::Fwd) or in-degree (Dir::Rev) of every node.
    [[nodiscard]] std::span<const uint32_t> degrees(Dir direction) const;

    /**
     * Decode every neighbour list of one direction into plain CSR arrays, e.g. straight into GPU buffers.
     *
     * The list of node u is targets[offsets[u] .. offsets[u + 1]), sorted. Reads the list file directly, in
     * parallel, so it works under any RAM budget. Throws if the graph has 2^32 links or more, or on I/O errors.
     * @param offsets    n + 1 entries
     * @param targets    info().edges entries
     */
    void export_csr(Dir direction, std::span<uint32_t> offsets, std::span<uint32_t> targets) const;

    /// Random article pairs with at least one link in and one out, for benchmarks and profiling.
    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> random_pairs(size_t count, uint64_t seed) const;

    /// Run `queries` random searches and return block keys ordered by how many searches read them.
    std::vector<uint64_t> profile_blocks(size_t queries, uint64_t seed);

    /// The RAM a graph directory needs, without opening it.
    static GraphFootprint footprint_of(const std::filesystem::path& dir);

   private:
    struct Impl;
    /// The open graph; throws std::logic_error if none is open.
    [[nodiscard]] Impl& impl();
    [[nodiscard]] const Impl& impl() const;
    std::unique_ptr<Impl> impl_;
    GraphInfo info_;
    std::mutex search_mutex_;
};

}  // namespace wg
