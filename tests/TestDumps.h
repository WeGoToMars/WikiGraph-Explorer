#pragma once

// Helpers that write small synthetic Wikipedia dumps, so tests can build graphs of any shape.

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Graph/GraphBuilder.h"

namespace wg::test {

/// A fresh, empty directory under the system temp folder, removed when the object goes away.
class TempDir {
   public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                std::format("wikigraph-test-{}-{}", std::random_device{}(), counter.fetch_add(1));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

   private:
    std::filesystem::path path_;
};

struct TestPage {
    uint32_t id;
    std::string title;  ///< database form (underscores)
    bool redirect = false;
    int32_t ns = 0;
};

struct TestLink {
    uint32_t from;       ///< page_id of the source
    std::string target;  ///< title of the target
    int32_t from_ns = 0;
};

struct TestRedirect {
    uint32_t from;  ///< page_id of the redirect page
    std::string target;
    int32_t ns = 0;
    std::string interwiki{};
};

struct TestWiki {
    std::vector<TestPage> pages;
    std::vector<TestLink> links;
    std::vector<TestRedirect> redirects;
    bool one_row_per_line = true;  ///< the dump layout published since 2026-10; false: one INSERT line per table
};

inline std::string sql_string(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'' || c == '\\') out += '\\';
        out += c;
    }
    return out + "'";
}

inline void write_gz(const std::filesystem::path& path, const std::string& text) {
    gzFile f = gzopen(path.string().c_str(), "wb");
    if (f == nullptr) throw std::runtime_error("cannot create " + path.string());
    const int written = gzwrite(f, text.data(), static_cast<unsigned>(text.size()));
    gzclose(f);
    if (written != static_cast<int>(text.size())) throw std::runtime_error("cannot write " + path.string());
}

/// One table in mysqldump form: a CREATE TABLE naming `columns`, then the rows.
inline std::string dump_text(std::string_view table, const std::vector<std::string_view>& columns,
                             const std::vector<std::string>& rows, bool one_row_per_line) {
    std::string text = std::format("-- synthetic test dump\n\nCREATE TABLE `{}` (\n", table);
    for (const auto c : columns) text += std::format("  `{}` int NOT NULL,\n", c);
    text += std::format("  PRIMARY KEY (`{}`)\n) ENGINE=InnoDB;\n\n", columns.front());
    if (!rows.empty()) {
        text += std::format("INSERT INTO `{}` VALUES", table);
        for (size_t i = 0; i < rows.size(); ++i) {
            text += one_row_per_line ? "\n" : (i == 0 ? " " : "");
            text += rows[i];
            text += i + 1 < rows.size() ? "," : ";\n";
        }
    }
    return text + "UNLOCK TABLES;\n";
}

/// Write the four dumps of `wiki` as <dir>/<prefix>-<table>.sql.gz and return their paths.
inline DumpFiles write_wiki(const std::filesystem::path& dir, const TestWiki& wiki,
                            const std::string& prefix = "testwiki-20260101") {
    auto path = [&](std::string_view table) { return dir / std::format("{}-{}.sql.gz", prefix, table); };

    std::vector<std::string> rows;
    for (const auto& p : wiki.pages)
        rows.push_back(
            std::format("({},{},{},{},0,0.5,'20260101000000')", p.id, p.ns, sql_string(p.title), p.redirect ? 1 : 0));
    write_gz(path("page"), dump_text("page", {"page_id", "page_namespace", "page_title", "page_is_redirect"}, rows,
                                     wiki.one_row_per_line));

    // One link target per distinct title, in order of first use
    std::vector<std::string> targets;
    auto target_id = [&](const std::string& title) -> uint64_t {
        for (size_t i = 0; i < targets.size(); ++i)
            if (targets[i] == title) return i + 1;
        targets.push_back(title);
        return targets.size();
    };
    // pagelinks is sorted by source namespace, as in the real dumps (the builder relies on it to stop early)
    std::vector<const TestLink*> sorted;
    for (const auto& l : wiki.links) sorted.push_back(&l);
    std::ranges::stable_sort(sorted, {}, &TestLink::from_ns);
    rows.clear();
    for (const TestLink* l : sorted)
        rows.push_back(std::format("({},{},{})", l->from, l->from_ns, target_id(l->target)));
    write_gz(path("pagelinks"),
             dump_text("pagelinks", {"pl_from", "pl_from_namespace", "pl_target_id"}, rows, wiki.one_row_per_line));

    rows.clear();
    for (size_t i = 0; i < targets.size(); ++i) rows.push_back(std::format("({},0,{})", i + 1, sql_string(targets[i])));
    write_gz(path("linktarget"),
             dump_text("linktarget", {"lt_id", "lt_namespace", "lt_title"}, rows, wiki.one_row_per_line));

    rows.clear();
    for (const auto& r : wiki.redirects)
        rows.push_back(std::format("({},{},{},{},'')", r.from, r.ns, sql_string(r.target), sql_string(r.interwiki)));
    write_gz(path("redirect"),
             dump_text("redirect", {"rd_from", "rd_namespace", "rd_title", "rd_interwiki", "rd_fragment"}, rows,
                       wiki.one_row_per_line));

    return {path("page"), path("linktarget"), path("pagelinks"), path("redirect")};
}

}  // namespace wg::test
