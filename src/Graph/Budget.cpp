#include "Graph/Budget.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "Graph/Platform.h"

namespace wg {

std::optional<uint64_t> parse_budget(std::string_view text) {
    std::string s = text | std::views::transform([](unsigned char c) { return static_cast<char>(std::tolower(c)); }) |
                    std::ranges::to<std::string>();
    if (s == "auto") return kBudgetAuto;
    if (s == "all" || s == "max") return kBudgetAll;
    constexpr std::array<std::pair<std::string_view, double>, 4> kUnits = {
        {{"gb", kBytesPerGB}, {"mb", kBytesPerMB}, {"g", kBytesPerGB}, {"m", kBytesPerMB}}};
    double scale = kBytesPerGB;  // bare numbers are GB
    if (const auto unit = std::ranges::find_if(kUnits, [&](const auto& u) { return s.ends_with(u.first); });
        unit != kUnits.end()) {
        scale = unit->second;
        s.resize(s.size() - unit->first.size());
    }
    // std::from_chars for double needs macOS 26 in libc++; stod is locale-dependent but only runs before the
    // locale is set (command line parsing)
    double v = 0;
    size_t used = 0;
    try {
        v = std::stod(s, &used);
    } catch (const std::logic_error&) {  // invalid_argument or out_of_range
        return std::nullopt;
    }
    const double bytes = v * scale;
    if (used != s.size() || !(bytes >= 1) || bytes >= 0x1p64) return std::nullopt;  // 0x1p64: past uint64_t
    const auto budget = static_cast<uint64_t>(bytes);
    if (budget == kBudgetAuto || budget == kBudgetAll) return std::nullopt;
    return budget;
}

std::string budget_to_string(uint64_t budget) {
    if (budget == kBudgetAuto) return "auto";
    if (budget == kBudgetAll) return "all";
    return std::format("{:.2f} GB", static_cast<double>(budget) / kBytesPerGB);
}

uint64_t resolve_budget(uint64_t requested, uint64_t needed) {
    if (requested == kBudgetAll) return needed;
    if (requested != kBudgetAuto) return requested;
    const uint64_t physical = physical_memory_bytes();
    return physical == 0 ? needed : std::min(needed, physical / 2);
}

}  // namespace wg
