// End-to-end tests of the core: dumps -> build_graph -> GraphStore -> search.

#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <random>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Graph/Budget.h"
#include "Graph/GraphBuilder.h"
#include "Graph/GraphFormat.h"
#include "Graph/GraphStore.h"
#include "Graph/PathSearch.h"
#include "TestDumps.h"

using wg::test::TempDir;
using wg::test::TestWiki;

namespace {

constexpr unsigned kThreads = 4;

/// The CI's synthetic wiki, copied into `dir` (the build writes its graph next to the dumps).
wg::DumpFiles copy_ci_wiki(const std::filesystem::path& dir) {
    const std::filesystem::path src = WG_TEST_WIKI_DIR;
    auto copy = [&](std::string_view table) {
        const auto name = std::format("testwiki-20260101-{}.sql.gz", table);
        std::filesystem::copy_file(src / name, dir / name);
        return dir / name;
    };
    return {copy("page"), copy("linktarget"), copy("pagelinks"), copy("redirect")};
}

wg::BuildReport build(const wg::DumpFiles& dumps, const std::filesystem::path& dir) {
    return wg::build_graph(dumps, dir, kThreads, {});
}

uint32_t node(const wg::GraphStore& g, std::string_view title) {
    const auto m = g.find(title);
    REQUIRE(m.has_value());
    return m->node;
}

std::set<std::vector<std::string>> titled(const wg::GraphStore& g, const wg::PathResult& r) {
    std::set<std::vector<std::string>> out;
    for (const auto& path : r.paths) {
        std::vector<std::string> titles;
        titles.reserve(path.size());
        for (const uint32_t u : path) titles.emplace_back(g.title(u));
        out.insert(std::move(titles));
    }
    return out;
}

/// The three ways a graph can be opened: every list in RAM, a hot set of blocks, every list read from disk.
uint64_t budget_for(std::string_view mode, const std::filesystem::path& dir) {
    if (mode == "all") return wg::kBudgetAll;
    const auto [fixed, lists] = wg::GraphStore::footprint_of(dir);
    if (mode == "hot") return fixed + wg::kBlockSize;  // one of the blocks
    return 1;                                          // below the fixed cost: no list is cached
}

}  // namespace

TEST_CASE("The CI wiki builds into the expected graph") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    const auto rep = build(dumps, dir);

    // Start, Alpha, Beta, Gamma's_Page, Target, Isolated; Old_Name is folded into Beta; Talk:Start is not main
    CHECK(rep.articles == 6);
    CHECK(rep.redirects == 1);
    CHECK(rep.redirects_unresolved == 0);
    // Start->Alpha, Start->Beta (via Old_Name), Alpha->Gamma, Beta->Gamma, Gamma->Target, Target->Start
    CHECK(rep.edges == 6);
    CHECK(rep.duplicate_links == 0);
    CHECK(wg::graph_is_current(dir, dumps));
}

TEST_CASE("Searching the CI wiki gives the same answers under every RAM budget") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);

    const auto mode = GENERATE(as<std::string>{}, "all", "hot", "disk");
    CAPTURE(mode);
    wg::GraphStore g;
    g.open(dir, budget_for(mode, dir));
    CHECK(g.info().all_in_ram == (mode == "all"));

    const auto r = g.shortest_paths(node(g, "Start"), node(g, "Target"), 10);
    CHECK(r.distance == 3);
    CHECK(r.path_count == 2);
    CHECK(titled(g, r) == std::set<std::vector<std::string>>{{"Start", "Alpha", "Gamma's_Page", "Target"},
                                                             {"Start", "Beta", "Gamma's_Page", "Target"}});
    if (mode == "disk") CHECK(r.blocks_read > 0);

    CHECK(g.shortest_paths(node(g, "Target"), node(g, "Start"), 10).distance == 1);
    CHECK(g.shortest_paths(node(g, "Start"), node(g, "Isolated"), 10).distance == -1);

    const auto self = g.shortest_paths(node(g, "Alpha"), node(g, "Alpha"), 10);
    CHECK(self.distance == 0);
    CHECK(self.path_count == 1);
    CHECK(self.paths.size() == 1);
}

TEST_CASE("find() normalises what the user typed and follows redirects") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);
    wg::GraphStore g;
    g.open(dir, wg::kBudgetAll);

    const auto start = g.find("Start");
    REQUIRE(start);
    CHECK_FALSE(start->via_redirect);
    CHECK(g.find("start").value().node == start->node);  // first letter in either case
    CHECK(g.find("  Start \t").value().node == start->node);
    CHECK(g.title(g.find("Gamma's Page").value().node) == "Gamma's_Page");

    const auto old = g.find("Old Name");
    REQUIRE(old);
    CHECK(old->via_redirect);
    CHECK(g.title(old->node) == "Beta");

    CHECK_FALSE(g.find("Missing_Page"));
    CHECK_FALSE(g.find(""));
    CHECK_FALSE(g.find("   "));
}

TEST_CASE("complete() puts the exact match first, then the most linked-to prefix matches") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);
    wg::GraphStore g;
    g.open(dir, wg::kBudgetAll);

    const auto gamma = g.complete("ga", 5);
    REQUIRE(gamma.size() == 1);
    CHECK(g.title(gamma[0]) == "Gamma's_Page");

    const auto start = g.complete("Start", 5);
    REQUIRE_FALSE(start.empty());
    CHECK(g.title(start[0]) == "Start");

    CHECK(g.complete("Zz", 5).empty());
    CHECK(g.complete("A", 0).empty());
}

TEST_CASE("Redirects: chains are followed, cycles and dangling or interwiki targets are dropped") {
    TempDir tmp;
    TestWiki w;
    w.pages = {{1, "A"},        {2, "B"},        {3, "C"},        {4, "R1", true}, {5, "R2", true},
               {6, "R3", true}, {7, "R4", true}, {8, "R5", true}, {9, "R6", true}, {10, "A", false, 1}};  // Talk:A
    w.redirects = {{4, "B"}, {5, "R1"}, {6, "R4"}, {7, "R3"}, {8, "Missing"}, {9, "Foo", 0, "en"}};
    w.links = {
        {1, "B"},     {1, "R2"},  // R2 -> R1 -> B: a duplicate of A -> B
        {1, "R3"},                // a redirect cycle: dropped
        {4, "C"},                 // from a redirect page: dropped
        {2, "C"},                 //
        {2, "B"},                 // self-link: dropped
        {2, "R1"},                // folds to a self-link: dropped
        {10, "C", 1},             // from the talk namespace: dropped
    };
    const auto dumps = wg::test::write_wiki(tmp.path(), w);
    const auto dir = tmp.path() / "g.graph";
    const auto rep = build(dumps, dir);

    CHECK(rep.pages == 9);
    CHECK(rep.articles == 3);
    CHECK(rep.redirects == 6);
    CHECK(rep.redirects_unresolved == 4);  // R3, R4 (cycle), R5 (missing), R6 (interwiki)
    CHECK(rep.edges == 2);                 // A -> B, B -> C
    CHECK(rep.duplicate_links == 1);

    wg::GraphStore g;
    g.open(dir, wg::kBudgetAll);
    CHECK(g.title(g.find("R2").value().node) == "B");
    CHECK(g.find("R2").value().via_redirect);
    CHECK_FALSE(g.find("R3"));
    CHECK_FALSE(g.find("R6"));
    CHECK(g.shortest_paths(node(g, "A"), node(g, "C"), 10).distance == 2);
}

TEST_CASE("A cancelled build throws BuildCancelled and leaves nothing behind") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_AS(wg::build_graph(dumps, dir, kThreads, {}, stop.get_token()), wg::BuildCancelled);
    CHECK_FALSE(std::filesystem::exists(dir));
    CHECK_FALSE(std::filesystem::exists(dir.string() + ".tmp"));
}

TEST_CASE("ensure_graph builds once, then reuses the graph") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    CHECK(wg::graph_dir_for(dumps) == tmp.path() / "testwiki-20260101.graph");
    wg::BuildReport rep;
    CHECK(wg::ensure_graph(dumps, false, kThreads, {}, &rep));
    CHECK(rep.articles == 6);
    CHECK_FALSE(wg::ensure_graph(dumps, false, kThreads, {}, &rep));
    CHECK(wg::ensure_graph(dumps, true, kThreads, {}, &rep));
}

TEST_CASE("export_csr matches the degrees") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);
    wg::GraphStore g;
    g.open(dir, 1);  // export reads the files directly, under any budget
    const uint32_t n = g.info().nodes;
    for (const wg::Dir d : wg::kDirs) {
        std::vector<uint32_t> offsets(n + 1), targets(g.info().edges);
        g.export_csr(d, offsets, targets);
        const auto deg = g.degrees(d);
        for (uint32_t u = 0; u < n; ++u) {
            CHECK(offsets[u + 1] - offsets[u] == deg[u]);
            CHECK(std::is_sorted(targets.begin() + offsets[u], targets.begin() + offsets[u + 1]));
        }
    }
}

// ---------------------------------------------------------------------------------------------------- random graphs

namespace {

struct Reference {
    std::vector<std::set<uint32_t>> out;  ///< by page index (page_id - 1), deduplicated, no self-links

    /// Distance and number of shortest paths from s to t (-1 and 0 when unreachable).
    [[nodiscard]] std::pair<int32_t, uint64_t> search(uint32_t s, uint32_t t) const {
        std::vector<int32_t> dist(out.size(), -1);
        std::vector<uint64_t> count(out.size(), 0);
        std::deque<uint32_t> queue{s};
        dist[s] = 0;
        count[s] = 1;
        while (!queue.empty()) {
            const uint32_t u = queue.front();
            queue.pop_front();
            for (const uint32_t v : out[u]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    queue.push_back(v);
                }
                if (dist[v] == dist[u] + 1) count[v] += count[u];
            }
        }
        return {dist[t], count[t]};
    }
};

}  // namespace

TEST_CASE("Search agrees with a plain BFS on random graphs, under every RAM budget") {
    const auto seed = GENERATE(1u, 2u, 3u);
    const bool one_row_per_line = seed != 2;
    CAPTURE(seed);
    std::mt19937 rng(seed);
    const uint32_t n = 400;
    const size_t links = 1600;

    TestWiki w;
    w.one_row_per_line = one_row_per_line;
    for (uint32_t i = 1; i <= n; ++i) w.pages.push_back({i, std::format("P{}", i)});
    Reference ref;
    ref.out.resize(n);
    std::uniform_int_distribution<uint32_t> pick(0, n - 1);
    for (size_t k = 0; k < links; ++k) {
        // Skewed towards low ids, so there are hubs and many equal-length paths; duplicates and self-links included
        const uint32_t u = std::min(pick(rng), pick(rng)), v = std::min(pick(rng), pick(rng));
        w.links.push_back({u + 1, std::format("P{}", v + 1)});
        if (u != v) ref.out[u].insert(v);
    }

    TempDir tmp;
    const auto dumps = wg::test::write_wiki(tmp.path(), w);
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);

    for (const std::string_view mode : {"all", "hot", "disk"}) {
        CAPTURE(mode);
        wg::GraphStore g;
        g.open(dir, budget_for(mode, dir));
        std::vector<uint32_t> node_of(n);
        for (uint32_t i = 0; i < n; ++i) node_of[i] = node(g, std::format("P{}", i + 1));

        for (int q = 0; q < 150; ++q) {
            const uint32_t s = pick(rng), t = pick(rng);
            const auto [dist, count] = ref.search(s, t);
            const auto r = g.shortest_paths(node_of[s], node_of[t], 50);
            CAPTURE(s, t);
            REQUIRE(r.distance == dist);
            REQUIRE(r.path_count == count);
            REQUIRE(r.paths.size() == std::min<uint64_t>(count, 50));

            std::set<std::vector<uint32_t>> distinct(r.paths.begin(), r.paths.end());
            REQUIRE(distinct.size() == r.paths.size());
            for (const auto& path : r.paths) {
                REQUIRE(path.size() == static_cast<size_t>(dist) + 1);
                REQUIRE(path.front() == node_of[s]);
                REQUIRE(path.back() == node_of[t]);
                for (size_t i = 0; i + 1 < path.size(); ++i) {
                    // Back from graph nodes to page indexes via the titles ("P<id>")
                    const auto a = std::stoul(std::string(g.title(path[i]).substr(1))) - 1;
                    const auto b = std::stoul(std::string(g.title(path[i + 1]).substr(1))) - 1;
                    REQUIRE(ref.out[a].contains(static_cast<uint32_t>(b)));
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------------- failures

TEST_CASE("Queries on a store with no graph open throw instead of crashing") {
    wg::GraphStore g;
    CHECK_FALSE(g.is_open());
    CHECK_THROWS_AS(g.find("Start"), std::logic_error);
    CHECK_THROWS_AS(g.complete("St", 5), std::logic_error);
    CHECK_THROWS_AS(g.title(0), std::logic_error);
    CHECK_THROWS_AS(g.display_title(0), std::logic_error);
    CHECK_THROWS_AS(g.shortest_paths(0, 1, 10), std::logic_error);
    CHECK_THROWS_AS(g.random_pairs(10, 1), std::logic_error);
    CHECK_THROWS_AS(g.degrees(wg::Dir::Fwd), std::logic_error);
    CHECK_THROWS_AS(g.profile_blocks(10, 1), std::logic_error);
    std::vector<uint32_t> offsets(1), targets;
    CHECK_THROWS_AS(g.export_csr(wg::Dir::Fwd, offsets, targets), std::logic_error);
}

TEST_CASE("A graph with a truncated list file is refused when opened") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);
    std::filesystem::resize_file(dir / "rev.lists", std::filesystem::file_size(dir / "rev.lists") - 1);
    for (const uint64_t budget : {wg::kBudgetAll, uint64_t{1}}) {
        wg::GraphStore g;
        CHECK_THROWS_AS(g.open(dir, budget), std::runtime_error);
        CHECK_FALSE(g.is_open());
    }
}

TEST_CASE("A list file truncated while the graph is open fails the search instead of giving wrong answers") {
    TempDir tmp;
    const auto dumps = copy_ci_wiki(tmp.path());
    const auto dir = tmp.path() / "g.graph";
    build(dumps, dir);
    wg::GraphStore g;
    g.open(dir, 1);  // every list read from disk
    const uint32_t s = node(g, "Start"), t = node(g, "Target");
    REQUIRE(g.shortest_paths(s, t, 10).distance == 3);

    std::filesystem::resize_file(dir / "fwd.lists", 0);
    std::filesystem::resize_file(dir / "rev.lists", 0);
    CHECK_THROWS_AS(g.shortest_paths(s, t, 10), std::runtime_error);
}
