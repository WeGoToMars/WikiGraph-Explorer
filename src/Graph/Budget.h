#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The RAM a graph may use (--ram-budget): a byte count, or one of the two selectors below.
namespace wg {

/// Decimal units, as everywhere in the program's output: 1 GB = 10^9 bytes.
inline constexpr double kBytesPerGB = 1e9;
inline constexpr double kBytesPerMB = 1e6;

inline constexpr uint64_t kBudgetAuto = 0;          ///< everything if it fits in half of physical memory, else half
inline constexpr uint64_t kBudgetAll = UINT64_MAX;  ///< always load every list

/// Parse "2.5" (GB), "2.5G", "800MB", "auto" or "all"; nothing on bad input.
std::optional<uint64_t> parse_budget(std::string_view text);

/// Human-readable budget, e.g. "auto", "all", "2.50 GB".
std::string budget_to_string(uint64_t budget);

/// The byte count a requested budget stands for, for a graph that needs `needed` bytes to be fully in RAM.
uint64_t resolve_budget(uint64_t requested, uint64_t needed);

}  // namespace wg
