#include "FetchWikiStats.h"

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <utility>

#include "Http.h"

/// Parse CSV text (RFC 4180): quoted fields may contain commas, doubled quotes and line breaks.
static std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.empty()) {
                row.push_back(std::move(field));
                rows.push_back(std::move(row));
            }
            field.clear();
            row.clear();
            any = false;
        } else {
            field.push_back(c);
            any = true;
        }
    }
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

/// UTF-8 encoding of a code point (std::codecvt is deprecated).
static std::string to_utf8(char32_t cp) {
    if (cp < 0x80) return {static_cast<char>(cp)};
    std::string out;
    const int n = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;  // bytes
    out.push_back(static_cast<char>((0xF00u >> n) | (cp >> (6 * (n - 1)))));
    for (int i = n - 2; i >= 0; --i) out.push_back(static_cast<char>(0x80u | ((cp >> (6 * i)) & 0x3Fu)));
    return out;
}

/**
 * @brief Decode the HTML character references WikiStats puts in language names ("&#1080;", "&#x6f;", "&ograve;").
 * Anything else is left as it is.
 */
static void decode_html_entities(std::string& str) {
    // The named ones that occur in the list
    static constexpr std::array<std::pair<std::string_view, char32_t>, 23> kNamed = {{
        {"amp", U'&'},           {"sol", U'/'},           {"agrave", U'\u00E0'}, {"egrave", U'\u00E8'},
        {"igrave", U'\u00EC'},   {"ograve", U'\u00F2'},   {"imath", U'\u0131'},  {"scedil", U'\u015F'},
        {"zcaron", U'\u017E'},   {"acy", U'\u0430'},      {"vcy", U'\u0432'},    {"dcy", U'\u0434'},
        {"iecy", U'\u0435'},     {"zhcy", U'\u0436'},     {"kcy", U'\u043A'},    {"lcy", U'\u043B'},
        {"mcy", U'\u043C'},      {"ncy", U'\u043D'},      {"ocy", U'\u043E'},    {"scy", U'\u0441'},
        {"ucy", U'\u0443'},      {"ycy", U'\u044B'},      {"jsercy", U'\u0458'},
    }};
    std::string out;
    for (size_t i = 0; i < str.size();) {
        const size_t end = str[i] == '&' ? str.find(';', i) : std::string::npos;
        const std::string_view ref =
            end == std::string::npos ? std::string_view{} : std::string_view(str).substr(i + 1, end - i - 1);
        char32_t cp = 0;
        if (ref.starts_with('#')) {
            const bool hex = ref.starts_with("#x") || ref.starts_with("#X");
            const std::string_view digits = ref.substr(hex ? 2 : 1);
            uint32_t value = 0;
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
            if (ec == std::errc{} && ptr == digits.data() + digits.size() && value <= 0x10FFFF) cp = value;
        } else if (const auto it = std::ranges::find(kNamed, ref, &std::pair<std::string_view, char32_t>::first);
                   it != kNamed.end()) {
            cp = it->second;
        }
        if (cp == 0) {
            out.push_back(str[i++]);
        } else {
            out += to_utf8(cp);
            i = end + 1;
        }
    }
    str = std::move(out);
}

/// Leaves `out` unchanged unless the whole field is a number.
static void parse_int(std::string_view field, int& out) {
    std::from_chars(field.data(), field.data() + field.size(), out);
}

std::vector<WikiEntry> fetch_wiki_stats(std::stop_token stop) {
    std::vector<WikiEntry> wiki_stats;

    const cpr::Response r =
        cpr::Get(cpr::Url{"https://wikistats.wmcloud.org/api.php?action=dump&table=wikipedias&format=csv"},
                 wiki_http::user_agent(), cpr::Timeout{wiki_http::kRequestTimeout}, wiki_http::cancel_callback(std::move(stop)));
    if (r.error || r.status_code != 200) {
        spdlog::warn("Could not fetch wiki statistics (HTTP {} {}); continuing offline", r.status_code, r.error.message);
        return wiki_stats;
    }

    const auto rows = parse_csv(r.text);
    if (rows.empty()) return wiki_stats;

    // Look columns up by name, so a new or reordered column upstream does not shift every field
    auto column = [&](std::string_view name) -> std::optional<size_t> {
        const auto it = std::ranges::find(rows[0], name);
        if (it == rows[0].end()) return std::nullopt;
        return static_cast<size_t>(it - rows[0].begin());
    };
    const auto c_lang = column("lang"), c_prefix = column("prefix"), c_good = column("good"), c_users = column("users"),
               c_local = column("loclang");
    if (!c_lang || !c_prefix || !c_good || !c_users || !c_local) {
        spdlog::error("Unexpected wiki statistics format; continuing offline");
        return wiki_stats;
    }
    const size_t needed = std::max({*c_lang, *c_prefix, *c_good, *c_users, *c_local}) + 1;

    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& fields = rows[i];
        if (fields.size() < needed) continue;
        WikiEntry e{.language_code = fields[*c_prefix],
                    .language_name = fields[*c_lang],
                    .local_language_name = fields[*c_local]};
        for (auto* name : {&e.language_name, &e.local_language_name}) {
            decode_html_entities(*name);
            // A name can contain line breaks (inside quotes); show it on one line
            std::ranges::replace_if(*name, [](char c) { return c == '\n' || c == '\r'; }, ' ');
        }
        parse_int(fields[*c_good], e.articles);
        parse_int(fields[*c_users], e.users);
        if (!e.language_code.empty()) wiki_stats.push_back(std::move(e));
    }

    // Sort by number of users, a good proxy for the size of the wiki
    std::ranges::sort(wiki_stats, std::greater{}, &WikiEntry::users);
    return wiki_stats;
}
