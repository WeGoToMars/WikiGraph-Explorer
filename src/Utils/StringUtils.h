#pragma once

#include <ranges>
#include <string>
#include <string_view>

namespace string_utils {

/// The elements of `items` (strings or string views) joined by `sep`: join({"a", "b"}, ", ") == "a, b".
template <std::ranges::input_range R>
std::string join(R&& items, std::string_view sep) {
    return std::forward<R>(items) | std::views::join_with(sep) | std::ranges::to<std::string>();
}

/// The whitespace trim() removes: ASCII only, so the bytes of UTF-8 characters are never taken for spaces.
inline constexpr std::string_view kWhitespace = " \t\n\v\f\r";

/// `text` without its leading whitespace.
constexpr std::string_view trim_start(std::string_view text) {
    const size_t first = text.find_first_not_of(kWhitespace);
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

/// `text` without its leading and trailing whitespace.
constexpr std::string_view trim(std::string_view text) {
    const std::string_view start = trim_start(text);
    return start.substr(0, start.find_last_not_of(kWhitespace) + 1);  // npos + 1 == 0 for an empty view
}

}  // namespace string_utils
