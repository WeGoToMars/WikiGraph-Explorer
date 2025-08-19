#pragma once
#include <charconv>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class SQLTupleParser {
   public:
    /**
     * @brief Construct a tuple parser over a single SQL VALUES tuple.
     * @param tuple String view of the tuple text
     */
    SQLTupleParser(std::string_view tuple);

    template <typename integer_type>
        requires std::is_integral_v<integer_type>
    /**
     * @brief Parse the next integer from the tuple.
     * @tparam integer_type Any integral type destination
     * @param value Reference receiving the parsed value
     * @return true on success, false if parsing failed or no number found
     */
    bool next_int(integer_type& value) {
        consume_delimiters();

        auto [ptr, err] = std::from_chars(m_tuple.data() + m_pos, m_tuple.data() + m_tuple.size(), value);
        if (err != std::errc()) {
            return false;
        }

        m_pos = ptr - m_tuple.data();
        return true;
    }

    /**
     * @brief Parse the next SQL-escaped string literal.
     * @param out Output string receiving the decoded contents
     * @return true on success, false if no string present
     */
    bool next_string(std::string& out);

    /**
     * @brief Parse the next boolean value.
     * @param value Output boolean
     * @return true on success, false otherwise
     */
    bool next_bool(bool& value);

   private:
    std::string_view m_tuple;
    size_t m_pos = 0;
    /**
     * @brief Advance parser position past commas, parentheses and whitespace.
     */
    void consume_delimiters();
};

/**
 * @brief Extract individual tuple substrings from an INSERT INTO line.
 * @param line Full SQL line starting with INSERT INTO ... VALUES (...),(...)
 * @return Views into the original string, one per tuple
 */
std::vector<std::string_view> extract_tuples(std::string_view line);

/**
 * @brief Estimate the number of tuples/items in a gzip file using first line size.
 * @param filename Path to compressed SQL dump
 * @param first_line_size Size in bytes of the first INSERT line
 * @return Estimated item count
 */
uint64_t estimated_number_of_items(const std::filesystem::path& filename, uint64_t first_line_size);
