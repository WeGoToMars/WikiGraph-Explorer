// RAM budget text: parse_budget() and budget_to_string().

#include <catch2/catch_test_macros.hpp>

#include "Graph/Budget.h"

TEST_CASE("parse_budget accepts the keywords, bare GB and unit suffixes") {
    CHECK(wg::parse_budget("auto") == wg::kBudgetAuto);
    CHECK(wg::parse_budget("AUTO") == wg::kBudgetAuto);
    CHECK(wg::parse_budget("all") == wg::kBudgetAll);
    CHECK(wg::parse_budget("max") == wg::kBudgetAll);
    CHECK(wg::parse_budget("2.5") == 2'500'000'000u);
    CHECK(wg::parse_budget("2G") == 2'000'000'000u);
    CHECK(wg::parse_budget("2gb") == 2'000'000'000u);
    CHECK(wg::parse_budget("800M") == 800'000'000u);
    CHECK(wg::parse_budget("800MB") == 800'000'000u);
}

TEST_CASE("parse_budget rejects bad input") {
    CHECK_FALSE(wg::parse_budget(""));
    CHECK_FALSE(wg::parse_budget("abc"));
    CHECK_FALSE(wg::parse_budget("1.5x"));
    CHECK_FALSE(wg::parse_budget("0"));
    CHECK_FALSE(wg::parse_budget("-1"));
    CHECK_FALSE(wg::parse_budget("1e30"));
}

TEST_CASE("budget_to_string") {
    CHECK(wg::budget_to_string(wg::kBudgetAuto) == "auto");
    CHECK(wg::budget_to_string(wg::kBudgetAll) == "all");
    CHECK(wg::budget_to_string(2'500'000'000u) == "2.50 GB");
}
