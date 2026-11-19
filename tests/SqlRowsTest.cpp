// The SQL row tokenizer in Graph/DumpReader.h: both dump layouts, string escapes, and rows the callers skip.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Graph/DumpReader.h"

namespace {

struct Row {
    uint64_t id = 0;
    int64_t ns = 0;
    std::string title;
};

/// Rows of the form (id, ns, 'title', ...), as the page table starts.
std::vector<Row> rows_of(std::string_view text) {
    std::vector<Row> out;
    wg::for_each_row(text, [&](wg::Cursor& c) {
        Row r;
        if (wg::parse_u64(c, r.id) && wg::expect(c, ',') && wg::parse_i64(c, r.ns) && wg::expect(c, ',') &&
            wg::parse_str(c, r.title))
            out.push_back(std::move(r));
    });
    return out;
}

}  // namespace

TEST_CASE("for_each_row reads one INSERT statement per line (dumps up to 2026-09)") {
    const auto rows = rows_of("INSERT INTO `page` VALUES (1,0,'April',0),(2,0,'August',1);\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == 1);
    CHECK(rows[0].title == "April");
    CHECK(rows[1].id == 2);
    CHECK(rows[1].title == "August");
}

TEST_CASE("for_each_row reads one row per line (dumps from 2026-10)") {
    const auto rows = rows_of("INSERT INTO `page` VALUES\n(1,0,'April',0),\n(2,0,'August',1),\n(3,4,'Help',0);\n");
    REQUIRE(rows.size() == 3);
    CHECK(rows[2].id == 3);
    CHECK(rows[2].ns == 4);
}

TEST_CASE("for_each_row ignores comments, DDL and lock statements") {
    const auto rows = rows_of(
        "-- MariaDB dump\n"
        "CREATE TABLE `page` (\n  `page_id` int,\n  PRIMARY KEY (`page_id`)\n);\n"
        "LOCK TABLES `page` WRITE;\n"
        "INSERT INTO `page` VALUES (7,0,'X',0);\n"
        "UNLOCK TABLES;\n");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == 7);
}

TEST_CASE("for_each_row survives quotes, parentheses and commas inside strings") {
    // The row rest after the title is skipped by skip_row_rest, which must honour the quoting
    const auto rows = rows_of(R"(INSERT INTO `t` VALUES (1,0,'A',' ),( \' ) '),(2,0,'B',NULL);)");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].title == "A");
    CHECK(rows[1].title == "B");
}

TEST_CASE("parse_str decodes MySQL escapes") {
    const auto rows = rows_of(R"(INSERT INTO `t` VALUES (1,0,'Gamma\'s_Page'),(2,0,'back\\slash'),(3,0,'tab\there');)");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].title == "Gamma's_Page");
    CHECK(rows[1].title == "back\\slash");
    CHECK(rows[2].title == "tab\there");
}

TEST_CASE("parse_str leaves the cursor in place on an unterminated string") {
    const std::string_view text = "'never closed";
    wg::Cursor c{text.data(), text.data() + text.size()};
    std::string out;
    CHECK_FALSE(wg::parse_str(c, out));
    CHECK(c.p == text.data());
}

TEST_CASE("parse_u64 and parse_i64") {
    auto cursor = [](std::string_view s) { return wg::Cursor{s.data(), s.data() + s.size()}; };
    uint64_t u = 0;
    int64_t i = 0;

    auto c = cursor("12345,");
    REQUIRE(wg::parse_u64(c, u));
    CHECK(u == 12345);
    CHECK(*c.p == ',');

    c = cursor("-42)");
    REQUIRE(wg::parse_i64(c, i));
    CHECK(i == -42);

    c = cursor("NULL");
    CHECK_FALSE(wg::parse_u64(c, u));
    CHECK(c.p == std::string_view("NULL").data());

    c = cursor("-x");
    const char* before = c.p;
    CHECK_FALSE(wg::parse_i64(c, i));
    CHECK(c.p == before);
}
