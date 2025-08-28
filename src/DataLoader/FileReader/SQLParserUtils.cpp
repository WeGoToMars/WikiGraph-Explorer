#include "SQLParserUtils.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/spdlog.h"

SQLTupleParser::SQLTupleParser(std::string_view tuple) : m_tuple(tuple) {};

void SQLTupleParser::consume_delimiters() {
    if (m_pos < m_tuple.size() && m_tuple[m_pos] == ',') {
        m_pos++;
    }
}

bool SQLTupleParser::next_string(std::string& out) {
    consume_delimiters();
    if (m_pos >= m_tuple.size() || m_tuple[m_pos] != '\'') {
        return false;  // malformed, no opening quote
    }

    const std::size_t start = ++m_pos;  // move to after the opening quote
    const std::size_t end = m_tuple.find('\'', start);
    if (end == std::string_view::npos) {
        return false;  // malformed, no closing quote
    }

    auto slice = m_tuple.substr(start, end - start);

    // Fast path – no escapes
    if (slice.find_first_of("\\'") == std::string_view::npos) {
        out.assign(slice);
        std::ranges::replace(out, '_', ' ');
        m_pos = end + 1;  // consume closing quote

        return true;
    }

    // Slow path - only if the string contains escapes
    out.clear();
    out.reserve(slice.size());
    bool escape = false;
    for (char c : slice) {
        if (escape) {
            if (c == '\'' || c == '\\') {
                out.push_back(c);
            }
            escape = false;
        } else if (c == '\\')
            escape = true;
        else if (c == '_')
            out.push_back(' ');
        else
            out.push_back(c);
    }
    m_pos = end + 1;

    return !escape;  // unmatched '\' ⇒ error
}

bool SQLTupleParser::next_bool(bool& value) {
    uint32_t int_val = 0;
    if (next_int(int_val)) {
        value = (int_val != 0);
        return true;
    }
    return false;
}

// Extracts all top-level SQL tuples from a given line of text.
std::vector<std::string_view> extract_tuples(std::string_view line) {
    // Strip leading "INSERT INTO ..." and trailing ");"
    line.remove_prefix(line.find('(') + 1);
    line.remove_suffix(2);

    constexpr std::string_view delim{"),("};

    // Reserve roughly – cheap upper bound.
    std::vector<std::string_view> tuples;
    tuples.reserve(std::ranges::count(line, '(') + 1);

    std::size_t start = 0;
    while (true) {
        std::size_t end = line.find(delim, start);
        if (end == std::string_view::npos) {  // no more tuples
            tuples.emplace_back(line.substr(start));
            break;
        }
        tuples.emplace_back(line.substr(start, end - start));
        start = end + delim.size();
    }

    return tuples;
}

// Wikipedia SQL dumps are split into lines of 1MB (uncompressed)
uint64_t estimated_number_of_items(const std::filesystem::path& filename, const uint64_t first_line_size) {
    uint64_t file_size = std::filesystem::file_size(filename);

    std::ifstream file(filename, std::ios::binary);
    file.seekg(-4, std::ios::end);
    uint32_t original_size;
    file.read(reinterpret_cast<char*>(&original_size), 4);
    double compression_ratio = static_cast<double>(original_size) / file_size;

    constexpr uint64_t MB = 1024 * 1024;

    uint64_t estimated_number_of_items =
        static_cast<uint64_t>((static_cast<double>(file_size) / MB) * first_line_size * compression_ratio);

    spdlog::debug("Estimated number of items: {}, file_size: {}, first_line_size: {}", estimated_number_of_items,
                  original_size, first_line_size);

    return estimated_number_of_items;
}
