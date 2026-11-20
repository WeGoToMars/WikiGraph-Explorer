#include "Graph/GraphBuilder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "Graph/Budget.h"
#include "Graph/DumpReader.h"
#include "Graph/GraphFormat.h"
#include "Graph/GraphStore.h"
#include "Graph/Ids.h"
#include "Graph/Log.h"
#include "Graph/Parallel.h"
#include "Graph/Platform.h"

// The build, in the order it runs:
//   1. read_pages            page dump        -> PageTable: main-namespace pages, their titles, which are redirects
//   2. read_redirect_targets redirect dump    -> the page each redirect points at
//      number_articles                        -> Articles: dense numbers, each redirect folded into its article
//   3. read_link_targets     linktarget dump  -> the article each lt_id names
//   4. count_links           pagelinks dump   -> LinkCounts: degrees, and where the main-namespace rows end
//   5. fill_links            pagelinks again  -> LinkLists in both directions (read_links runs 4 and 5)
//   6. dedup, order_nodes, then write_lists, write_nodes, write_titles, write_redirects, write_meta, write_hot_blocks
// Each phase returns what the later ones need, and the tables are freed as soon as nothing needs them.

namespace wg {

BuildCancelled::~BuildCancelled() = default;

namespace {

using ProgressFn = std::function<void(const BuildProgress&)>;

/// What every phase needs: the dumps, the threads, progress reporting and cancellation.
class Build {
   public:
    Build(const DumpFiles& files, unsigned thread_count, const ProgressFn& progress, std::stop_token stop)
        : dumps(files),
          threads(std::max(1u, thread_count)),
          workers(threads > 2 ? threads - 1 : 1),
          progress_(progress),
          stop_(std::move(stop)) {}

    const DumpFiles& dumps;
    const unsigned threads;  ///< at least 1
    const unsigned workers;  ///< threads that parse dump chunks

    void check_cancel() const {
        if (stop_.stop_requested()) throw BuildCancelled();
    }

    /// Stream a dump to `on_chunk`, reporting progress as `step`. A cancelled read throws, so a phase never goes on
    /// with half-filled tables.
    DumpStreamStats stream(DumpFile& file, uint64_t stop_after, BuildStep step,
                           const std::function<void(DumpChunk&)>& on_chunk) {
        rows_ = 0;
        auto report = [this, step](uint64_t done, uint64_t total) {
            if (progress_) progress_({step, done, total, rows_.load(std::memory_order_relaxed)});
        };
        const DumpStreamStats stats = file.stream(stop_after, workers, on_chunk, report, stop_);
        check_cancel();
        return stats;
    }
    /// Open a dump that is read once, and stream it.
    DumpStreamStats stream(const std::filesystem::path& path, BuildStep step,
                           const std::function<void(DumpChunk&)>& on_chunk) {
        DumpFile file(path, workers);
        return stream(file, 0, step, on_chunk);
    }
    /// Called from the chunk handlers: rows parsed, for the progress report.
    void count_rows(uint64_t n) { rows_ += n; }
    [[nodiscard]] uint64_t rows() const { return rows_.load(); }

    static constexpr uint64_t kFinishSteps = 6;
    /// Sub-step `k` of kFinishSteps in BuildStep::Finish.
    void finish_step(uint64_t k) const {
        check_cancel();
        if (progress_) progress_({BuildStep::Finish, k, kFinishSteps, 0});
    }

   private:
    const ProgressFn& progress_;
    std::stop_token stop_;
    std::atomic<uint64_t> rows_{0};
};

// ------------------------------------------------------------------------------------------------ files

/// Write the bytes of a contiguous range of plain values (a vector, string, span, ...).
template <std::ranges::contiguous_range R>
    requires std::is_trivially_copyable_v<std::ranges::range_value_t<R>>
void write_all(std::ofstream& f, const R& items, const std::filesystem::path& path) {
    const auto bytes = std::as_bytes(std::span(items));
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("cannot write " + path.string());
}

template <class T>
    requires std::is_trivially_copyable_v<T>
void write_value(std::ofstream& f, const T& value, const std::filesystem::path& path) {
    write_all(f, std::span(&value, 1), path);
}

uint64_t size_or_zero(const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty()) return 0;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : s;
}

/// GraphMeta::source_size: a graph is current while its dumps keep these sizes.
std::array<uint64_t, 4> source_sizes(const DumpFiles& dumps) {
    return {size_or_zero(dumps.page), size_or_zero(dumps.linktarget), size_or_zero(dumps.pagelinks),
            size_or_zero(dumps.redirect)};
}

bool has_redirect_dump(const DumpFiles& dumps) {
    return !dumps.redirect.empty() && std::filesystem::exists(dumps.redirect);
}

/// The rows are read by position: fail early, with a clear message, if a table's leading columns ever change.
void check_dump_columns(const DumpFiles& dumps) {
    check_columns(dumps.page, {"page_id", "page_namespace", "page_title", "page_is_redirect"});
    check_columns(dumps.linktarget, {"lt_id", "lt_namespace", "lt_title"});
    check_columns(dumps.pagelinks, {"pl_from", "pl_from_namespace", "pl_target_id"});
    if (has_redirect_dump(dumps))
        check_columns(dumps.redirect, {"rd_from", "rd_namespace", "rd_title", "rd_interwiki", "rd_fragment"});
}

/// The graph is written here, then renamed to its directory once complete.
std::filesystem::path temp_dir_for(const std::filesystem::path& dir) { return dir.string() + ".tmp"; }

// ------------------------------------------------------------------------------------------------ 1. pages

/// Main-namespace pages, articles and redirects alike, in page dump order, with their titles in one arena.
struct PageTable {
    IdVector<PageRow, PageId> page_id;
    IdVector<PageRow, uint8_t> is_redirect;  // 0 or 1
    std::vector<uint32_t> title_offset;      // row r's title is title_bytes[title_offset[r] .. title_offset[r + 1])
    std::string title_bytes;
    IdVector<PageId, PageRow> row_of;  // kNoId for ids that are not main-namespace pages

    [[nodiscard]] size_t size() const { return page_id.size(); }
    [[nodiscard]] auto rows() const { return page_id.ids(); }
    [[nodiscard]] bool redirect(PageRow r) const { return is_redirect[r] != 0; }
    [[nodiscard]] std::string_view title(PageRow r) const {
        const uint32_t i = raw(r);
        return {title_bytes.data() + title_offset[i], title_offset[i + 1] - title_offset[i]};
    }
};

/// The main-namespace rows of one chunk of the page dump.
struct PageChunk {
    std::vector<uint32_t> id, title_end;
    std::vector<uint8_t> redirect;
    std::string titles;
};

/// page_id, page_namespace, page_title, page_is_redirect
PageChunk parse_page_chunk(std::string_view text, uint64_t& rows) {
    PageChunk b;
    for_each_row(text, [&](Cursor& cur) {
        ++rows;
        uint64_t id = 0, redirect = 0;
        int64_t ns = 0;
        if (!parse_u64(cur, id) || !expect(cur, ',') || !parse_i64(cur, ns) || !expect(cur, ',')) return;
        if (ns != 0) return;
        const size_t before = b.titles.size();
        if (!parse_str(cur, b.titles) || !expect(cur, ',') || !parse_u64(cur, redirect)) {
            b.titles.resize(before);
            return;
        }
        b.id.push_back(static_cast<uint32_t>(id));
        b.redirect.push_back(redirect != 0 ? 1 : 0);
        b.title_end.push_back(static_cast<uint32_t>(b.titles.size()));
    });
    return b;
}

/// The chunks, in file order, as one table. Frees the chunks as it goes.
PageTable merge_page_chunks(std::vector<PageChunk>& chunks, const std::filesystem::path& dump) {
    size_t n = 0, bytes = 0;
    for (const auto& b : chunks) {
        n += b.id.size();
        bytes += b.titles.size();
    }
    if (n == 0) throw std::runtime_error("no main-namespace pages found in " + dump.string());
    if (bytes > UINT32_MAX) throw std::runtime_error("title data over 4 GB is not supported");
    PageTable pages;
    pages.page_id.reserve(n);
    pages.is_redirect.reserve(n);
    pages.title_offset.reserve(n + 1);
    pages.title_bytes.reserve(bytes);
    pages.title_offset.push_back(0);
    uint32_t max_id = 0;
    for (auto& b : chunks) {
        const auto base = static_cast<uint32_t>(pages.title_bytes.size());
        pages.title_bytes += b.titles;
        for (const auto [id, redirect, end] : std::views::zip(b.id, b.redirect, b.title_end)) {
            pages.page_id.push_back(PageId{id});
            pages.is_redirect.push_back(redirect);
            pages.title_offset.push_back(base + end);
            max_id = std::max(max_id, id);
        }
        b = PageChunk();
    }
    pages.row_of.assign(size_t{max_id} + 1, kNoId<PageRow>);
    for (const PageRow r : pages.rows()) pages.row_of[pages.page_id[r]] = r;
    return pages;
}

PageTable read_pages(Build& build) {
    std::mutex mu;
    std::vector<PageChunk> chunks;  // by position in the file, so the rows keep the dump's order
    auto on_chunk = [&](DumpChunk& c) {
        uint64_t rows = 0;
        PageChunk b = parse_page_chunk(c.text(), rows);
        build.count_rows(rows);
        std::lock_guard lk(mu);
        if (chunks.size() <= c.seq) chunks.resize(c.seq + 1);
        chunks[c.seq] = std::move(b);
    };
    build.stream(build.dumps.page, BuildStep::Pages, on_chunk);
    return merge_page_chunks(chunks, build.dumps.page);
}

/// Page rows by title: an open-addressing hash table over the titles of a PageTable.
class TitleHash {
   public:
    explicit TitleHash(const PageTable& pages) : pages_(pages) {
        const size_t capacity = std::bit_ceil(pages.size() * 2 + 16);
        slots_.assign(capacity, kNoId<PageRow>);
        mask_ = capacity - 1;
        for (const PageRow r : pages.rows()) {
            uint64_t h = hash(pages.title(r)) & mask_;
            while (slots_[h] != kNoId<PageRow>) h = (h + 1) & mask_;
            slots_[h] = r;
        }
    }

    /// The page with this title (database form), or kNoId.
    [[nodiscard]] PageRow find(std::string_view title) const {
        for (uint64_t h = hash(title) & mask_;; h = (h + 1) & mask_) {
            const PageRow r = slots_[h];
            if (r == kNoId<PageRow> || pages_.title(r) == title) return r;
        }
    }

   private:
    static uint64_t hash(std::string_view s) { return std::hash<std::string_view>{}(s); }

    const PageTable& pages_;
    std::vector<PageRow> slots_;
    uint64_t mask_ = 0;
};

// ------------------------------------------------------------------------------------------------ 2. redirects

/// rd_from, rd_namespace, rd_title, rd_interwiki, rd_fragment: for each redirect to a main-namespace page of this
/// wiki, set target[the redirect's row] to the row of the page it names.
void parse_redirect_chunk(std::string_view text, const PageTable& pages, const TitleHash& by_title,
                          IdVector<PageRow, PageRow>& target, uint64_t& rows) {
    std::string title, interwiki, fragment;
    for_each_row(text, [&](Cursor& cur) {
        ++rows;
        uint64_t from = 0;
        int64_t ns = 0;
        title.clear();
        interwiki.clear();
        fragment.clear();
        if (!parse_u64(cur, from) || !expect(cur, ',') || !parse_i64(cur, ns) || !expect(cur, ',') ||
            !parse_str(cur, title))
            return;
        if (expect(cur, ',') && parse_str(cur, interwiki) && expect(cur, ',')) parse_str(cur, fragment);
        const PageRow r = pages.row_of.get(from, kNoId<PageRow>);
        if (r == kNoId<PageRow> || !interwiki.empty() || ns != 0) return;
        target[r] = by_title.find(title);  // each redirect page has one row, so no two threads write one entry
    });
}

/// The page each redirect points at, by row; kNoId where the redirect dump names none.
IdVector<PageRow, PageRow> read_redirect_targets(Build& build, const PageTable& pages, const TitleHash& by_title) {
    IdVector<PageRow, PageRow> target(pages.size(), kNoId<PageRow>);
    if (!has_redirect_dump(build.dumps)) {
        log_warn("No redirect dump: links through redirects will be dropped");
        return target;
    }
    auto on_chunk = [&](DumpChunk& c) {
        uint64_t rows = 0;
        parse_redirect_chunk(c.text(), pages, by_title, target, rows);
        build.count_rows(rows);
    };
    build.stream(build.dumps.redirect, BuildStep::Redirects, on_chunk);
    return target;
}

/// The graph's nodes before they are put in locality order: the articles, numbered densely in page dump order.
struct Articles {
    IdVector<Article, PageRow> row;     // the page of each article
    IdVector<PageRow, Article> of_row;  // an article's own number; for a redirect, the article it finally points at
                                        // (kNoId if that is unknown)
    uint64_t redirects = 0, unresolved = 0;

    [[nodiscard]] size_t size() const { return row.size(); }
};

/// Redirects to redirects are followed up to this many times.
constexpr int kMaxRedirectHops = 8;

/// Number the articles, and fold each redirect into the article it finally points at.
Articles number_articles(const PageTable& pages, const IdVector<PageRow, PageRow>& target) {
    Articles a;
    a.of_row.assign(pages.size(), kNoId<Article>);
    for (const PageRow r : pages.rows())
        if (!pages.redirect(r)) {
            a.of_row[r] = a.row.next_id();
            a.row.push_back(r);
        }
    for (const PageRow r : pages.rows()) {
        if (!pages.redirect(r)) continue;
        PageRow to = target[r];
        for (int hops = 0; to != kNoId<PageRow> && pages.redirect(to) && hops < kMaxRedirectHops; ++hops)
            to = target[to];
        // Still a redirect: a cycle, or a chain too long
        const bool resolved = to != kNoId<PageRow> && !pages.redirect(to);
        a.of_row[r] = resolved ? a.of_row[to] : kNoId<Article>;
        ++a.redirects;
        if (!resolved) ++a.unresolved;
    }
    return a;
}

// ------------------------------------------------------------------------------------------------ 3. link targets

/// lt_id, lt_namespace, lt_title: the (lt_id, article) pairs of the targets that name an article or a resolved
/// redirect, and the largest lt_id in the chunk.
uint32_t parse_link_target_chunk(std::string_view text, const TitleHash& by_title, const Articles& articles,
                                 std::vector<std::pair<LinkTargetId, Article>>& hits, uint64_t& rows) {
    std::string title;
    uint32_t max_id = 0;
    for_each_row(text, [&](Cursor& cur) {
        ++rows;
        uint64_t id = 0;
        int64_t ns = 0;
        if (!parse_u64(cur, id) || !expect(cur, ',') || !parse_i64(cur, ns) || !expect(cur, ',')) return;
        max_id = std::max(max_id, static_cast<uint32_t>(id));
        if (ns != 0) return;
        title.clear();
        if (!parse_str(cur, title)) return;
        const PageRow r = by_title.find(title);
        if (r != kNoId<PageRow> && articles.of_row[r] != kNoId<Article>)
            hits.emplace_back(static_cast<LinkTargetId>(id), articles.of_row[r]);
    });
    return max_id;
}

/// The article each lt_id names; kNoId for other namespaces and for missing pages.
IdVector<LinkTargetId, Article> read_link_targets(Build& build, const TitleHash& by_title, const Articles& articles) {
    std::mutex mu;
    std::vector<std::vector<std::pair<LinkTargetId, Article>>> hits;
    uint32_t max_id = 0;
    auto on_chunk = [&](DumpChunk& c) {
        std::vector<std::pair<LinkTargetId, Article>> h;
        uint64_t rows = 0;
        const uint32_t chunk_max = parse_link_target_chunk(c.text(), by_title, articles, h, rows);
        build.count_rows(rows);
        std::lock_guard lk(mu);
        max_id = std::max(max_id, chunk_max);
        hits.push_back(std::move(h));
    };
    build.stream(build.dumps.linktarget, BuildStep::LinkTargets, on_chunk);
    IdVector<LinkTargetId, Article> article(size_t{max_id} + 1, kNoId<Article>);
    for (const auto& [id, a] : hits | std::views::join) article[id] = a;
    return article;
}

// ------------------------------------------------------------------------------------------------ 4, 5. links

/// The article of each page id that links can come from. Links from redirect pages are dropped.
IdVector<PageId, Article> source_articles(const PageTable& pages, const Articles& articles) {
    IdVector<PageId, Article> source(pages.row_of.size(), kNoId<Article>);
    for (const PageRow r : pages.rows())
        if (!pages.redirect(r)) source[pages.page_id[r]] = articles.of_row[r];
    return source;
}

/// A pagelinks row: pl_from, pl_from_namespace, pl_target_id.
struct PagelinksRow {
    uint64_t from = 0;
    int64_t ns = 0;
    uint64_t target = 0;
};

inline bool parse_pagelinks_row(Cursor& cur, PagelinksRow& row) {
    return parse_u64(cur, row.from) && expect(cur, ',') && parse_i64(cur, row.ns) && expect(cur, ',') &&
           parse_u64(cur, row.target);
}

struct Link {
    Article from, to;
};

/// Turns pagelinks rows into links between articles.
struct LinkEnds {
    IdVector<PageId, Article> source;
    IdVector<LinkTargetId, Article> target;

    /// The link a row makes: from a main-namespace article to another article.
    [[nodiscard]] std::optional<Link> link(const PagelinksRow& row) const {
        if (row.ns != 0) return std::nullopt;
        const Article from = source.get(row.from, kNoId<Article>);
        const Article to = target.get(row.target, kNoId<Article>);
        if (from == kNoId<Article> || to == kNoId<Article> || from == to) return std::nullopt;
        return Link{from, to};
    }
};

/// The namespaces of the rows of one pagelinks chunk. The dump has so far always been sorted by namespace first,
/// which lets pass 2 stop after the main-namespace rows.
struct ChunkNamespaces {
    int64_t first = -1, last = -1;  // -1: no rows
    bool sorted = true;             // never decreasing within the chunk
    bool has_main = false;
    uint64_t end = 0;  // uncompressed offset just past the chunk

    void add(int64_t ns) {
        if (first == -1) first = ns;
        if (last != -1 && ns < last) sorted = false;
        last = ns;
        has_main |= ns == 0;
    }
};

struct LinkCounts {
    IdVector<Article, uint32_t> out, in;  // degrees, duplicates included
    uint64_t links = 0;                   // rows that make a link
    uint64_t rows = 0;
    bool ns_sorted = false;   // every row of the dump is in namespace order
    uint64_t read_until = 0;  // where pass 2 can stop: the end of the main-namespace rows if sorted, else 0 (all)
};

/// Pass 1 over one chunk: count the degrees. Returns the links counted.
uint64_t count_chunk(const DumpChunk& c, const LinkEnds& ends, LinkCounts& counts, ChunkNamespaces& ns,
                     uint64_t& rows) {
    uint64_t links = 0;
    Article run = kNoId<Article>;  // rows arrive grouped by target: batch the in-degree increments
    uint32_t run_length = 0;
    auto flush = [&] {
        if (run != kNoId<Article> && run_length != 0)
            std::atomic_ref(counts.in[run]).fetch_add(run_length, std::memory_order_relaxed);
        run_length = 0;
    };
    for_each_row(c.text(), [&](Cursor& cur) {
        ++rows;
        PagelinksRow row;
        if (!parse_pagelinks_row(cur, row)) return;
        ns.add(row.ns);
        const auto link = ends.link(row);
        if (!link) return;
        ++links;
        std::atomic_ref(counts.out[link->from]).fetch_add(1, std::memory_order_relaxed);
        if (link->to != run) {
            flush();
            run = link->to;
        }
        ++run_length;
    });
    flush();
    return links;
}

/// Pass 1: degrees, and where the main-namespace rows end.
LinkCounts count_links(Build& build, DumpFile& pagelinks, const LinkEnds& ends, size_t articles) {
    LinkCounts counts;
    counts.out.assign(articles, 0);
    counts.in.assign(articles, 0);
    std::vector<ChunkNamespaces> chunk_ns;
    std::mutex mu;
    std::atomic<uint64_t> links{0};
    auto on_chunk = [&](DumpChunk& c) {
        ChunkNamespaces ns;
        ns.end = c.start + c.data.size();
        uint64_t rows = 0;
        links += count_chunk(c, ends, counts, ns, rows);
        build.count_rows(rows);
        std::lock_guard lk(mu);
        if (chunk_ns.size() <= c.seq) chunk_ns.resize(c.seq + 1);
        chunk_ns[c.seq] = ns;
    };
    const auto stats = build.stream(pagelinks, 0, BuildStep::LinksCount, on_chunk);
    counts.rows = build.rows();
    counts.links = links.load();
    if (counts.links == 0)
        throw std::runtime_error("no links between main-namespace pages found in " + pagelinks.path().string());

    // Stop pass 2 early only if pass 1 saw the namespace order hold over the whole file
    counts.ns_sorted = true;
    uint64_t main_end = 0;
    int64_t prev = -1;
    for (const auto& ns : chunk_ns) {
        if (ns.first == -1) continue;
        if (!ns.sorted || ns.first < prev) counts.ns_sorted = false;
        prev = ns.last;
        if (ns.has_main) main_end = ns.end;
    }
    counts.read_until = counts.ns_sorted ? main_end : 0;
    log_info("pagelinks pass 1: {} rows, {} links, {:.1f} GB uncompressed in {:.1f} s, namespace-sorted: {}",
             counts.rows, counts.links, static_cast<double>(stats.uncompressed_bytes) / kBytesPerGB, stats.seconds,
             counts.ns_sorted);
    return counts;
}

/// The links of one direction, compressed sparse rows: the list of u is target[offset[u] .. offset[u + 1]).
struct LinkLists {
    std::vector<uint64_t> offset;  // one per article, plus the end
    std::vector<Article> target;

    [[nodiscard]] size_t size() const { return offset.size() - 1; }
    [[nodiscard]] uint64_t degree(Article u) const { return offset[raw(u) + 1] - offset[raw(u)]; }
    [[nodiscard]] std::span<const Article> list(Article u) const {
        return std::span(target).subspan(offset[raw(u)], degree(u));
    }
    [[nodiscard]] std::span<Article> list(Article u) { return std::span(target).subspan(offset[raw(u)], degree(u)); }
};

/// Offsets for lists of these lengths, with one entry more for the end.
std::vector<uint64_t> offsets_for(std::span<const uint32_t> degree) {
    std::vector<uint64_t> offset(degree.size() + 1, 0);
    for (size_t u = 0; u < degree.size(); ++u) offset[u + 1] = offset[u] + degree[u];
    return offset;
}

/// Pass 2: read the links again and put each in both directions' lists.
PerDir<LinkLists> fill_links(Build& build, DumpFile& pagelinks, const LinkEnds& ends, LinkCounts counts) {
    PerDir<LinkLists> lists;
    lists[Dir::Fwd].offset = offsets_for(counts.out.values());
    lists[Dir::Rev].offset = offsets_for(counts.in.values());
    counts.out.release();  // before the link arrays below are allocated
    counts.in.release();
    for (const Dir d : kDirs) lists[d].target.resize(counts.links);

    // The next free slot of each list
    std::vector<uint64_t> out_at(lists[Dir::Fwd].offset.begin(), lists[Dir::Fwd].offset.end() - 1);
    std::vector<uint64_t> in_at(lists[Dir::Rev].offset.begin(), lists[Dir::Rev].offset.end() - 1);
    std::atomic<uint64_t> filled{0};
    auto on_chunk = [&](DumpChunk& c) {
        uint64_t links = 0, rows = 0;
        for_each_row(c.text(), [&](Cursor& cur) {
            ++rows;
            PagelinksRow row;
            if (!parse_pagelinks_row(cur, row)) return;
            const auto link = ends.link(row);
            if (!link) return;
            const uint64_t out_slot = std::atomic_ref(out_at[raw(link->from)]).fetch_add(1, std::memory_order_relaxed);
            const uint64_t in_slot = std::atomic_ref(in_at[raw(link->to)]).fetch_add(1, std::memory_order_relaxed);
            lists[Dir::Fwd].target[out_slot] = link->to;
            lists[Dir::Rev].target[in_slot] = link->from;
            ++links;
        });
        build.count_rows(rows);
        filled += links;
    };
    const auto stats = build.stream(pagelinks, counts.read_until, BuildStep::LinksFill, on_chunk);
    if (filled.load() != counts.links) throw std::runtime_error("pagelinks changed between the two passes");
    log_info("pagelinks pass 2: {:.1f} GB uncompressed in {:.1f} s",
             static_cast<double>(stats.uncompressed_bytes) / kBytesPerGB, stats.seconds);
    return lists;
}

/// Passes 1 and 2. The dump is opened once for both: with rapidgzip, pass 2 reuses the index of the file that pass 1
/// built. It is closed on return, with its index and decompression threads.
PerDir<LinkLists> read_links(Build& build, const LinkEnds& ends, size_t articles, uint64_t& rows) {
    DumpFile pagelinks(build.dumps.pagelinks, build.workers);
    LinkCounts counts = count_links(build, pagelinks, ends, articles);
    rows = counts.rows;
    return fill_links(build, pagelinks, ends, std::move(counts));
}

// ------------------------------------------------------------------------------------------------ 6. finish

/// Sort each list and drop repeated links (two redirects to one article make the same link twice, for example).
/// Returns the number of links left.
uint64_t dedup(LinkLists& lists, unsigned threads) {
    const auto n = static_cast<uint32_t>(lists.size());
    std::vector<uint32_t> length(n);
    const size_t parts = size_t{threads} * 16;
    parallel_for(parts, threads, [&](size_t k) {
        const auto lo = static_cast<uint32_t>(uint64_t{n} * k / parts);
        const auto hi = static_cast<uint32_t>(uint64_t{n} * (k + 1) / parts);
        for (uint32_t u = lo; u < hi; ++u) {
            const std::span<Article> list = lists.list(static_cast<Article>(u));
            std::ranges::sort(list);
            length[u] = static_cast<uint32_t>(std::ranges::unique(list).begin() - list.begin());
        }
    });
    // Close the gaps, in place: each list moves left, so copying forwards is safe
    uint64_t w = 0;
    for (uint32_t u = 0; u < n; ++u) {
        const uint64_t r = lists.offset[u];
        lists.offset[u] = w;
        if (w != r) std::copy_n(lists.target.data() + r, length[u], lists.target.data() + w);
        w += length[u];
    }
    lists.offset[n] = w;
    lists.target.resize(w);  // the capacity is kept; the lists are freed right after they are written
    return w;
}

/// The node numbers: a locality order of the articles.
struct NodeOrder {
    IdVector<Node, Article> article;
    IdVector<Article, Node> node;
};

/// The top 1% of articles by degree first, then breadth-first over links in both directions, started from each
/// article in order of degree. Lists read together then tend to share blocks, and the gaps in them stay small.
NodeOrder order_nodes(const PerDir<LinkLists>& links) {
    const size_t n = links[Dir::Fwd].size();
    auto degree = [&](Article u) { return links[Dir::Fwd].degree(u) + links[Dir::Rev].degree(u); };
    // size_t, not uint32_t: see IdVector::ids()
    auto by_degree = std::views::iota(size_t{0}, n) |
                     std::views::transform([](size_t u) { return static_cast<Article>(u); }) |
                     std::ranges::to<std::vector>();
    std::ranges::sort(by_degree,
                      [&](Article a, Article b) { return degree(a) != degree(b) ? degree(a) > degree(b) : a < b; });

    NodeOrder order;
    order.article.reserve(n);
    IdVector<Article, uint8_t> seen(n, 0);
    auto visit = [&](Article u) {
        if (seen[u] != 0) return;
        seen[u] = 1;
        order.article.push_back(u);
    };
    for (const Article u : by_degree | std::views::take(n / 100)) visit(u);
    size_t head = 0;
    for (const Article root : by_degree) {
        visit(root);
        for (; head < order.article.size(); ++head) {
            const Article u = order.article[static_cast<Node>(head)];
            for (const Dir d : kDirs)
                for (const Article v : links[d].list(u)) visit(v);
        }
    }
    order.node.assign(n, kNoId<Node>);
    for (const Node v : order.article.ids()) order.node[order.article[v]] = v;
    return order;
}

/// Append a sorted list: the first id, then the gaps, each in LEB128. Returns the bytes appended.
size_t encode_list(std::span<const uint32_t> sorted, std::vector<uint8_t>& out) {
    const size_t before = out.size();
    out.resize(before + sorted.size() * kVarintMaxBytes);
    uint8_t* p = out.data() + before;
    uint32_t prev = 0;
    for (const uint32_t id : sorted) {
        uint32_t x = id - prev;  // the first: id - 0
        while (x > kVarintPayload) {
            *p++ = static_cast<uint8_t>(x | kVarintMore);
            x >>= kVarintPayloadBits;
        }
        *p++ = static_cast<uint8_t>(x);
        prev = id;
    }
    const auto written = static_cast<size_t>(p - (out.data() + before));
    out.resize(before + written);
    return written;
}

/// One direction's lists in node order, as written: their degrees and byte offsets go in nodes.bin.
struct ListIndex {
    IdVector<Node, uint32_t> degree;
    std::vector<uint64_t> offset;  // one per node, plus the end (the file size)
};

/// Write one direction's lists in node order (fwd.lists or rev.lists). Ranges of nodes are encoded in parallel and
/// written in order.
ListIndex write_lists(const Build& build, const LinkLists& links, const NodeOrder& order,
                      const std::filesystem::path& path) {
    constexpr uint32_t kNodesPerPart = 1u << 16;
    const auto n = static_cast<uint32_t>(order.article.size());
    const unsigned threads = build.threads;
    ListIndex index{.degree = IdVector<Node, uint32_t>(n), .offset = std::vector<uint64_t>(n + 1ull)};
    std::ofstream out(path, std::ios::binary);
    const uint32_t parts = (n + kNodesPerPart - 1) / kNodesPerPart;
    uint64_t at = 0;
    for (uint32_t first = 0; first < parts; first += threads * 2) {
        build.check_cancel();
        const uint32_t count = std::min<uint32_t>(threads * 2, parts - first);
        std::vector<std::vector<uint8_t>> bytes(count);
        std::vector<std::vector<uint32_t>> lengths(count);
        parallel_for(count, threads, [&](size_t k) {
            const uint32_t lo = (first + static_cast<uint32_t>(k)) * kNodesPerPart;
            const uint32_t hi = std::min(n, lo + kNodesPerPart);
            std::vector<uint32_t> ids;
            for (uint32_t i = lo; i < hi; ++i) {
                const auto node = static_cast<Node>(i);
                ids.clear();
                for (const Article v : links.list(order.article[node])) ids.push_back(raw(order.node[v]));
                std::ranges::sort(ids);
                lengths[k].push_back(static_cast<uint32_t>(encode_list(ids, bytes[k])));
                index.degree[node] = static_cast<uint32_t>(ids.size());
            }
        });
        for (uint32_t k = 0; k < count; ++k) {
            uint64_t p = at;
            const uint32_t lo = (first + k) * kNodesPerPart;
            for (size_t j = 0; j < lengths[k].size(); ++j) {
                index.offset[lo + j] = p;
                p += lengths[k][j];
            }
            write_all(out, bytes[k], path);
            at += bytes[k].size();
        }
    }
    index.offset[n] = at;
    return index;
}

void write_nodes(const std::filesystem::path& path, const PerDir<ListIndex>& index) {
    std::ofstream out(path, std::ios::binary);
    for (const Dir d : kDirs) write_all(out, index[d].degree.values(), path);
    for (const Dir d : kDirs) write_all(out, index[d].offset, path);
}

/// titles.bin: the titles in node order, then the node ids sorted by title. Returns the title bytes.
uint64_t write_titles(const std::filesystem::path& path, const PageTable& pages, const Articles& articles,
                      const NodeOrder& order) {
    const size_t n = order.article.size();
    std::vector<uint32_t> offset(n + 1, 0);
    std::string bytes;
    for (const Node v : order.article.ids()) {
        bytes += pages.title(articles.row[order.article[v]]);
        offset[raw(v) + 1] = static_cast<uint32_t>(bytes.size());
    }
    std::vector<uint32_t> by_title(n);
    std::ranges::iota(by_title, 0u);
    auto title = [&](uint32_t v) { return std::string_view(bytes.data() + offset[v], offset[v + 1] - offset[v]); };
    std::ranges::sort(by_title, {}, title);
    std::ofstream out(path, std::ios::binary);
    write_all(out, offset, path);
    write_all(out, bytes, path);
    write_all(out, by_title, path);
    return bytes.size();
}

/// redirects.bin and redirects.idx: the resolved redirects sorted by title, with a sparse index for one-read
/// lookups. Returns the number of redirects written.
uint64_t write_redirects(const std::filesystem::path& dir, const PageTable& pages, const Articles& articles,
                         const NodeOrder& order) {
    std::vector<PageRow> redirects;
    for (const PageRow r : pages.rows())
        if (pages.redirect(r) && articles.of_row[r] != kNoId<Article> && pages.title(r).size() <= UINT16_MAX)
            redirects.push_back(r);
    std::ranges::sort(redirects, {}, [&](PageRow r) { return pages.title(r); });

    const auto path = dir / graph_file::kRedirects, index_path = dir / graph_file::kRedirectIndex;
    std::ofstream out(path, std::ios::binary), index(index_path, std::ios::binary);
    const uint64_t count = redirects.size();
    write_value(index, count, index_path);
    uint64_t at = 0;
    for (size_t i = 0; i < redirects.size(); ++i) {
        const std::string_view title = pages.title(redirects[i]);
        const auto length = static_cast<uint16_t>(title.size());  // checked above
        const uint32_t to = raw(order.node[articles.of_row[redirects[i]]]);
        if (i % kRedirectStride == 0) {
            write_value(index, at, index_path);
            write_value(index, length, index_path);
            write_all(index, title, index_path);
        }
        write_value(out, length, path);
        write_all(out, title, path);
        write_value(out, to, path);
        at += kRedirectEntryOverhead + length;
    }
    return count;
}

void write_meta(const std::filesystem::path& dir, const GraphMeta& meta) {
    const auto path = dir / graph_file::kMeta;
    std::ofstream out(path, std::ios::binary);
    write_value(out, meta, path);
}

/// hot.bin: profile random searches with every list in RAM. The blocks they read most are the ones worth keeping
/// in RAM when the budget does not hold every list.
void write_hot_blocks(const Build& build, const std::filesystem::path& dir) {
    constexpr size_t kProfileSearches = 2000;
    constexpr uint64_t kProfileSeed = 0x5eed;
    GraphStore store;
    store.open(dir, kBudgetAll);
    build.check_cancel();
    const std::vector<uint64_t> keys = store.profile_blocks(kProfileSearches, kProfileSeed);
    const auto path = dir / graph_file::kHotBlocks;
    std::ofstream out(path, std::ios::binary);
    write_all(out, keys, path);
}

// ------------------------------------------------------------------------------------------------ the build

BuildReport build_graph_impl(const DumpFiles& dumps, const std::filesystem::path& dir, unsigned threads,
                             const ProgressFn& progress, std::stop_token stop) {
    const auto t0 = std::chrono::steady_clock::now();
    Build build(dumps, threads, progress, std::move(stop));
    log_info("Building graph {} with {} threads", dir.string(), build.threads);
    check_dump_columns(dumps);
    BuildReport rep;

    PageTable pages = read_pages(build);
    LinkEnds ends;
    Articles articles;
    {
        const TitleHash by_title(pages);
        articles = number_articles(pages, read_redirect_targets(build, pages, by_title));
        ends.target = read_link_targets(build, by_title, articles);
    }
    ends.source = source_articles(pages, articles);
    pages.row_of.release();
    rep.pages = pages.size();
    rep.articles = articles.size();
    rep.redirects = articles.redirects;
    rep.redirects_unresolved = articles.unresolved;

    PerDir<LinkLists> links = read_links(build, ends, articles.size(), rep.pagelinks_rows);
    const uint64_t links_read = links[Dir::Fwd].target.size();  // before dedup
    ends = LinkEnds();

    build.finish_step(0);
    rep.edges = dedup(links[Dir::Fwd], build.threads);
    if (dedup(links[Dir::Rev], build.threads) != rep.edges)
        throw std::runtime_error("internal error: link directions disagree");
    rep.duplicate_links = links_read - rep.edges;
    build.finish_step(1);
    const NodeOrder order = order_nodes(links);
    build.finish_step(2);

    const std::filesystem::path tmp = temp_dir_for(dir);
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    PerDir<ListIndex> index;
    for (const Dir d : kDirs) index[d] = write_lists(build, links[d], order, tmp / graph_file::lists(d));
    links = PerDir<LinkLists>();
    write_nodes(tmp / graph_file::kNodes, index);
    rep.list_bytes = index[Dir::Fwd].offset.back() + index[Dir::Rev].offset.back();
    build.finish_step(3);

    GraphMeta meta{};
    meta.magic = kGraphMagic;
    meta.version = kGraphVersion;
    meta.nodes = static_cast<uint32_t>(articles.size());
    meta.edges = rep.edges;
    meta.source_size = source_sizes(dumps);
    meta.list_bytes = {index[Dir::Fwd].offset.back(), index[Dir::Rev].offset.back()};
    meta.title_bytes = write_titles(tmp / graph_file::kTitles, pages, articles, order);
    meta.redirects = write_redirects(tmp, pages, articles, order);
    meta.redirects_unresolved = rep.redirects_unresolved;
    meta.duplicate_links = rep.duplicate_links;
    pages = PageTable();
    build.finish_step(4);

    write_meta(tmp, meta);  // the store opened to profile the searches reads it
    write_hot_blocks(build, tmp);
    build.finish_step(5);

    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    meta.build_seconds = rep.seconds;
    write_meta(tmp, meta);
    std::filesystem::remove_all(dir);
    std::filesystem::rename(tmp, dir);
    rep.peak_footprint = process_peak_footprint_bytes();
    build.finish_step(Build::kFinishSteps);
    log_info(
        "Built graph {}: {} articles, {} links ({} duplicates merged), {} redirects ({} unresolved), "
        "{:.1f} MB of lists, {:.1f} s",
        dir.string(), rep.articles, rep.edges, rep.duplicate_links, rep.redirects, rep.redirects_unresolved,
        static_cast<double>(rep.list_bytes) / kBytesPerMB, rep.seconds);
    return rep;
}

}  // namespace

const char* build_step_name(BuildStep step) {
    switch (step) {
        case BuildStep::Pages:
            return "Reading pages";
        case BuildStep::Redirects:
            return "Reading redirects";
        case BuildStep::LinkTargets:
            return "Reading link targets";
        case BuildStep::LinksCount:
            return "Reading links (counting)";
        case BuildStep::LinksFill:
            return "Reading links (building)";
        case BuildStep::Finish:
            return "Writing the graph";
    }
    std::unreachable();
}

std::filesystem::path graph_dir_for(const DumpFiles& dumps) {
    std::string name = dumps.page.filename().string();  // <lang>wiki-<date>-page.sql.gz
    if (const auto p = name.rfind("-page.sql.gz"); p != std::string::npos) name.resize(p);
    return dumps.page.parent_path() / (name + ".graph");
}

bool graph_is_current(const std::filesystem::path& dir, const DumpFiles& dumps) {
    std::ifstream f(dir / graph_file::kMeta, std::ios::binary);
    GraphMeta m{};
    if (!f.read(reinterpret_cast<char*>(&m), sizeof m)) return false;
    return m.magic == kGraphMagic && m.version == kGraphVersion && m.source_size == source_sizes(dumps);
}

BuildReport build_graph(const DumpFiles& dumps, const std::filesystem::path& dir, unsigned threads,
                        const ProgressFn& progress, std::stop_token stop) {
    try {
        return build_graph_impl(dumps, dir, threads, progress, std::move(stop));
    } catch (const BuildCancelled&) {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_for(dir), ec);
        log_info("Graph build cancelled: {}", dir.string());
        throw;
    }
}

bool ensure_graph(const DumpFiles& dumps, bool force, unsigned threads, const ProgressFn& progress, BuildReport* report,
                  std::stop_token stop) {
    const auto dir = graph_dir_for(dumps);
    if (!force && graph_is_current(dir, dumps)) return false;
    const BuildReport r = build_graph(dumps, dir, threads, progress, std::move(stop));
    if (report != nullptr) *report = r;
    return true;
}

}  // namespace wg
