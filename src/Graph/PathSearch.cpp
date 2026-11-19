#include "Graph/PathSearch.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ratio>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

#include "Graph/GraphFormat.h"
#include "Graph/NeighbourLists.h"

namespace wg {
namespace {

uint64_t sat_add(uint64_t a, uint64_t b) { return a + b < a ? UINT64_MAX : a + b; }
uint64_t sat_mul(uint64_t a, uint64_t b) { return b != 0 && a > UINT64_MAX / b ? UINT64_MAX : a * b; }

using Dag = std::vector<std::pair<uint32_t, uint32_t>>;

/// Every path from x to `end` along `dag` (sorted), each starting at x; stops once `out` holds `limit` paths.
void walk_dag(const Dag& dag, uint32_t x, uint32_t end, size_t limit, std::vector<std::vector<uint32_t>>& out) {
    std::vector<uint32_t> path{x};
    auto walk = [&](this const auto& self, uint32_t v) -> void {
        if (v == end) {
            out.push_back(path);
            return;
        }
        for (const auto& edge : std::ranges::equal_range(dag, v, {}, &Dag::value_type::first)) {
            if (out.size() >= limit) return;
            path.push_back(edge.second);
            self(edge.second);
            path.pop_back();
        }
    };
    walk(x);
}

}  // namespace

void PathSearch::Side::reset() {
    for (const uint32_t v : touched) {
        dist[v] = -1;
        count[v] = 0;
    }
    touched.clear();
    frontier.clear();
    dag.clear();
    depth = 0;
}

void PathSearch::Side::start(uint32_t root) {
    dist[root] = 0;
    count[root] = 1;
    touched.push_back(root);
    frontier.assign(1, root);
}

uint64_t PathSearch::Side::frontier_cost(const NeighbourLists& lists) const {
    const std::span<const uint32_t> degree = lists.degrees(dir);  // in RAM: known without I/O
    uint64_t links = 0;
    for (const uint32_t u : frontier) links += degree[u];
    return links;
}

// The arrays are allocated in the order, and (see GraphStore::open) at the point, that the search was tuned with:
// moving them changed search times with the same instructions executed (docs/PERF.md)
PathSearch::PathSearch(uint32_t nodes) : fwd_(Dir::Fwd), bwd_(Dir::Rev) {
    fwd_.dist.assign(nodes, -1);
    bwd_.dist.assign(nodes, -1);
    fwd_.count.assign(nodes, 0);
    bwd_.count.assign(nodes, 0);
}

PathResult PathSearch::run(NeighbourLists& lists, uint32_t from, uint32_t to, size_t max_paths,
                           const std::function<void(const SearchProgress&)>& progress, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    PathResult r;
    lists.begin_query();
    auto finish = [&] {
        r.blocks_read = lists.blocks_read();
        r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };
    const size_t n = fwd_.dist.size();
    if (from >= n || to >= n) return finish();
    fwd_.reset();
    bwd_.reset();
    fwd_.start(from);
    bwd_.start(to);
    if (from == to) {
        r.distance = 0;
        r.path_count = 1;
        if (max_paths > 0) r.paths.push_back({from});
        return finish();
    }
    while (!fwd_.frontier.empty() && !bwd_.frontier.empty()) {
        if (stop.stop_requested()) {
            r.cancelled = true;
            return finish();
        }
        const bool forward = fwd_.frontier_cost(lists) <= bwd_.frontier_cost(lists);
        Side& side = forward ? fwd_ : bwd_;
        const bool met = expand(side, forward ? bwd_ : fwd_, lists, r);
        if (progress) progress({fwd_.depth, bwd_.depth, side.frontier.size(), r.lists_expanded});
        if (met) {
            collect_paths(from, to, max_paths, r);
            return finish();
        }
    }
    return finish();  // a frontier ran out: no path
}

bool PathSearch::expand(Side& side, const Side& other, NeighbourLists& lists, PathResult& r) {
    // Locals for the loops, so the compiler can keep them in registers across the stores below
    const Dir dir = side.dir;
    const std::span<int32_t> dist = side.dist;
    const std::span<const int32_t> other_dist = other.dist;
    const std::span<uint64_t> count = side.count;
    std::vector<uint32_t>& touched = side.touched;
    Dag& dag = side.dag;
    const int32_t depth = side.depth;
    bool met = false;
    next_.clear();
    ++r.rounds;
    lists.fetch(dir, side.frontier);
    for (const uint32_t u : side.frontier) {
        const std::span<const uint32_t> neighbours = lists.neighbours(dir, u);
        ++r.lists_expanded;
        const auto len = static_cast<uint32_t>(neighbours.size());
        r.edges_scanned += len;
        const uint64_t paths_to_u = count[u];
        for (uint32_t i = 0; i < len; ++i) {  // indexed: faster than a range-for (docs/PERF.md)
            const uint32_t v = neighbours[i];
            if (dist[v] == -1) {  // first reached: on the next level
                dist[v] = depth + 1;
                count[v] = paths_to_u;
                touched.push_back(v);
                next_.push_back(v);
                dag.emplace_back(v, u);
                met |= other_dist[v] != -1;
            } else if (dist[v] == depth + 1) {  // reached again on the same level: another shortest path
                count[v] = sat_add(count[v], paths_to_u);
                dag.emplace_back(v, u);
            }
        }
    }
    side.frontier.swap(next_);
    ++side.depth;
    return met;
}

void PathSearch::collect_paths(uint32_t from, uint32_t to, size_t max_paths, PathResult& r) {
    // Every shortest path crosses the forward depth at exactly one node that is also at the backward depth
    r.distance = fwd_.depth + bwd_.depth;
    std::vector<uint32_t> meet;
    for (const uint32_t v : fwd_.touched)
        if (fwd_.dist[v] == fwd_.depth && bwd_.dist[v] == bwd_.depth) {
            meet.push_back(v);
            r.path_count = sat_add(r.path_count, sat_mul(fwd_.count[v], bwd_.count[v]));
        }
    if (max_paths == 0) return;

    std::ranges::sort(fwd_.dag);
    std::ranges::sort(bwd_.dag);
    std::ranges::sort(meet);
    std::vector<std::vector<uint32_t>> left, right;
    for (const uint32_t x : meet) {
        if (r.paths.size() >= max_paths) break;
        const size_t room = max_paths - r.paths.size();
        left.clear();
        right.clear();
        walk_dag(fwd_.dag, x, from, room, left);  // x back to the start
        walk_dag(bwd_.dag, x, to, room, right);   // x on to the end
        for (const auto& l : left)
            for (const auto& rt : right) {
                if (r.paths.size() >= max_paths) break;
                auto& p = r.paths.emplace_back(l.rbegin(), l.rend());
                p.insert(p.end(), rt.begin() + 1, rt.end());  // append_range needs libstdc++ 15
            }
    }
}

}  // namespace wg
