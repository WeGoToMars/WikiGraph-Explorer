#pragma once

#include <concepts>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wg {

/** @brief A piece of decompressed dump text that ends on a line boundary. */
struct DumpChunk {
    uint64_t seq = 0;    ///< position of the chunk in the file, 0-based
    uint64_t start = 0;  ///< uncompressed byte offset of the first byte
    std::vector<char> data;
    [[nodiscard]] std::string_view text() const { return {data.data(), data.size()}; }
};

struct DumpStreamStats {
    uint64_t uncompressed_bytes = 0;
    uint64_t chunks = 0;
    double seconds = 0;
    bool stopped_early = false;
    bool cancelled = false;  ///< stopped by a stop request; the data seen is incomplete
};

/**
 * @brief A .sql.gz dump opened for reading, possibly several times.
 *
 * Built with WG_PARALLEL_GZIP (the CMake option WIKIGRAPH_PARALLEL_GZIP), rapidgzip decompresses it on `threads`
 * threads and keeps an index of the deflate blocks it found: a later read starts every chunk at a known place with
 * a known window, and is about twice as fast. Otherwise zlib decompresses it on the reading thread. Nothing is
 * written to disk either way.
 */
class DumpFile {
   public:
    DumpFile(const std::filesystem::path& path, unsigned threads);
    ~DumpFile();
    DumpFile(const DumpFile&) = delete;
    DumpFile& operator=(const DumpFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    /**
     * @brief Decompress the dump from its start, in memory, and hand ~8 MB chunks of whole lines to worker threads.
     *
     * @param stop_after   stop after this many uncompressed bytes (0 = read everything)
     * @param threads      worker threads (at least 1)
     * @param on_chunk     called on worker threads, in any order
     * @param on_progress  called on the reading thread with (compressed bytes read, compressed file size)
     * @param stop         checked before each chunk; a stop request ends the reading (workers finish their chunk
     *                     first)
     */
    DumpStreamStats stream(uint64_t stop_after, unsigned threads, const std::function<void(DumpChunk&)>& on_chunk,
                           const std::function<void(uint64_t, uint64_t)>& on_progress, std::stop_token stop = {});

   private:
    class Input;
    std::filesystem::path path_;
    std::unique_ptr<Input> in_;
};

/// Open `path`, with `threads` decompression threads, and read it once (DumpFile::stream).
DumpStreamStats stream_dump(const std::filesystem::path& path, uint64_t stop_after, unsigned threads,
                            const std::function<void(DumpChunk&)>& on_chunk,
                            const std::function<void(uint64_t, uint64_t)>& on_progress, std::stop_token stop = {});

/**
 * @brief Check that the dump's CREATE TABLE statement starts with `leading` columns, in that order.
 *
 * The row tokenizer reads columns by position, so a schema change upstream would otherwise be read as wrong data.
 * Throws std::runtime_error naming the columns found when they differ (or when there is no CREATE TABLE).
 */
void check_columns(const std::filesystem::path& path, std::initializer_list<std::string_view> leading);

// ------------------------------------------------------------------------------------------------ row tokenizer
// mysqldump output in either layout:
//   INSERT INTO `t` VALUES (..),(..),(..);        one statement per line (dumps up to 2026-09)
//   INSERT INTO `t` VALUES\n(..),\n(..),\n(..);   one row per line     (dumps from 2026-10)
// Strings are single-quoted with backslash escapes; a raw newline never appears inside a row.

struct Cursor {
    const char* p;
    const char* end;
};

inline bool parse_u64(Cursor& c, uint64_t& v) {
    const char* p = c.p;
    if (p == c.end || *p < '0' || *p > '9') return false;
    uint64_t x = 0;
    while (p < c.end && *p >= '0' && *p <= '9') x = x * 10 + static_cast<uint64_t>(*p++ - '0');
    v = x;
    c.p = p;
    return true;
}

inline bool parse_i64(Cursor& c, int64_t& v) {
    Cursor t = c;
    const bool neg = t.p < t.end && *t.p == '-';
    if (neg) ++t.p;
    uint64_t u = 0;
    if (!parse_u64(t, u)) return false;
    v = neg ? -static_cast<int64_t>(u) : static_cast<int64_t>(u);
    c = t;
    return true;
}

inline bool expect(Cursor& c, char ch) {
    if (c.p < c.end && *c.p == ch) {
        ++c.p;
        return true;
    }
    return false;
}

/// Quoted string with backslash escapes; decoded bytes are appended to `out`. The cursor moves only on success.
inline bool parse_str(Cursor& c, std::string& out) {
    if (c.p >= c.end || *c.p != '\'') return false;
    const char* p = c.p + 1;
    for (;;) {
        const char* q = p;
        while (q < c.end && *q != '\'' && *q != '\\') ++q;
        if (q == c.end) return false;
        out.append(p, q);
        if (*q == '\'') {
            c.p = q + 1;
            return true;
        }
        if (q + 1 == c.end) return false;
        char e = q[1];
        switch (e) {
            case 'n': e = '\n'; break;
            case 'r': e = '\r'; break;
            case 't': e = '\t'; break;
            case '0': e = '\0'; break;
            case 'Z': e = '\x1a'; break;
            case 'b': e = '\b'; break;
            default: break;  // \' \" \\ and anything else: the character itself
        }
        out.push_back(e);
        p = q + 2;
    }
}

/// Skip to just past the ')' that closes the current row, honouring quoted strings.
inline bool skip_row_rest(Cursor& c) {
    const char* p = c.p;
    while (p < c.end) {
        const char ch = *p;
        if (ch == ')') {
            c.p = p + 1;
            return true;
        }
        if (ch == '\'') {
            ++p;
            while (p < c.end && *p != '\'') p += (*p == '\\') ? 2 : 1;
            if (p >= c.end) return false;
        }
        ++p;
    }
    return false;
}

/// Calls row(cursor) with the cursor just after '(' for every row in `text`.
template <class RowFn>
    requires std::invocable<RowFn&, Cursor&>
void for_each_row(std::string_view text, RowFn&& row) {
    const char* line = text.data();
    const char* const end = text.data() + text.size();
    while (line < end) {
        const auto* nl = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(end - line)));
        const char* le = nl != nullptr ? nl : end;
        const char* p = nullptr;
        if (le > line && *line == '(') {
            p = line;
        } else if (le - line >= 11 && std::memcmp(line, "INSERT INTO", 11) == 0) {
            const std::string_view head(line, std::min<size_t>(static_cast<size_t>(le - line), 256));
            const size_t v = head.find(" VALUES");
            if (v != std::string_view::npos) {
                p = line + v + 7;
                while (p < le && *p == ' ') ++p;
            }
        }
        if (p != nullptr) {
            Cursor c{p, le};
            while (c.p < c.end && *c.p == '(') {
                ++c.p;
                row(c);
                if (!skip_row_rest(c)) break;
                if (c.p < c.end && *c.p == ',') ++c.p;
            }
        }
        line = le + 1;
    }
}

}  // namespace wg
