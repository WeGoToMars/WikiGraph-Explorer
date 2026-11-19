#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>
#include <utility>
#include <vector>

#include "Graph/GraphFormat.h"

namespace wg {

class NeighbourLists;

struct SearchProgress {
    int32_t forward_depth = 0;
    int32_t backward_depth = 0;
    uint64_t frontier = 0;        ///< size of the frontier just produced
    uint64_t lists_expanded = 0;  ///< neighbour lists read so far
};

struct PathResult {
    int32_t distance = -1;                     ///< clicks, -1 if no path
    uint64_t path_count = 0;                   ///< number of shortest paths (UINT64_MAX if it saturated)
    std::vector<std::vector<uint32_t>> paths;  ///< up to the requested number of paths
    uint64_t lists_expanded = 0;
    uint64_t edges_scanned = 0;
    uint64_t blocks_read = 0;  ///< 16 KB blocks read from disk (0 when everything is in RAM)
    uint64_t rounds = 0;       ///< BFS levels expanded
    double milliseconds = 0;
    bool cancelled = false;
};

/// Bidirectional breadth-first search that counts every shortest path and lists some of them.
///
/// Each step expands the side whose frontier has fewer links to read. Both sides count the shortest paths from their
/// root to each node they reach and keep the edges that lie on such paths (a DAG), so once they meet, the number of
/// shortest paths is exact and paths can be listed by walking both DAGs from the nodes where they meet.
///
/// Holds the per-node state for one graph and reuses it from query to query; one query at a time.
class PathSearch {
   public:
    /// RAM the search state takes per node (distance and path count on each side).
    static constexpr uint64_t kBytesPerNode = 2 * (sizeof(int32_t) + sizeof(uint64_t));

    explicit PathSearch(uint32_t nodes);

    /**
     * @param max_paths  how many paths to list; the count is always exact (or saturated)
     * @param progress   called after each BFS level, may be empty
     * @param stop       checked once per level; a stop request ends the search with `cancelled` set
     */
    PathResult run(NeighbourLists& lists, uint32_t from, uint32_t to, size_t max_paths,
                   const std::function<void(const SearchProgress&)>& progress, std::stop_token stop);

   private:
    using Dag =
        std::vector<std::pair<uint32_t, uint32_t>>;  // edges (v, u): u is v's neighbour one step nearer the root

    /// One direction of the search: from the start along out-links, or from the end along in-links.
    struct Side {
        explicit Side(Dir d) : dir(d) {}
        void reset();               // forget the previous query (only the nodes it touched)
        void start(uint32_t root);  // the root is at depth 0, with one path
        [[nodiscard]] uint64_t frontier_cost(const NeighbourLists& lists) const;  // links to read to expand it

        Dir dir;
        std::vector<int32_t> dist;       // depth from the root, -1 if not reached
        std::vector<uint64_t> count;     // shortest paths from the root (saturating)
        std::vector<uint32_t> touched;   // every node reached, to reset
        std::vector<uint32_t> frontier;  // the nodes at the deepest level
        Dag dag;
        int32_t depth = 0;
    };

    /// Expand `side` by one level; true if it reached a node that `other` has reached.
    bool expand(Side& side, const Side& other, NeighbourLists& lists, PathResult& r);
    /// The searches met: count every shortest path and list up to `max_paths` of them.
    void collect_paths(uint32_t from, uint32_t to, size_t max_paths, PathResult& r);

    Side fwd_, bwd_;
    std::vector<uint32_t> next_;  // the frontier being built
};

}  // namespace wg
