#include "FetchWikiStats.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <ranges>
#include <sstream>

static std::vector<std::string> split_csv_line(const std::string& line) {
    auto fields = line | std::views::split(',') |
                  std::views::transform([](auto&& subrange) { return std::string(subrange.begin(), subrange.end()); });
    return {fields.begin(), fields.end()};
}

// Helper function needed because codecvt is deprecated since C++20
// Converts a Unicode code point to a sequence of UTF-8 bytes
// NOLINTBEGIN
static std::string append_utf8(uint32_t cp) {
    std::string out;
    // https://en.wikipedia.org/wiki/UTF-8#Description
    constexpr auto mask = (1 << 6) - 1;  // six bit mask

    if (cp <= 0x7F) {  // 1-byte (7 bit code point)
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {                                                // 2-byte (11 bit code point)
        out.push_back(static_cast<char>(0b11000000 | (cp >> 6)));            // get upper 5 bits
        out.push_back(static_cast<char>(0b10000000 | (cp & mask)));          // get lower 6 bits
    } else if (cp <= 0xFFFF) {                                               // 3-byte (16 bit code point)
        out.push_back(static_cast<char>(0b11100000 | (cp >> 12)));           // get upper 4 bits
        out.push_back(static_cast<char>(0b10000000 | ((cp >> 6) & mask)));   // get middle 6 bits
        out.push_back(static_cast<char>(0b10000000 | (cp & mask)));          // get lower 6 bits
    } else {                                                                 // 4-byte (max 0x10FFFF)
        out.push_back(static_cast<char>(0b11110000 | (cp >> 18)));           // get upper 3 bits
        out.push_back(static_cast<char>(0b10000000 | ((cp >> 12) & mask)));  // get middle-high 6 bits
        out.push_back(static_cast<char>(0b10000000 | ((cp >> 6) & mask)));   // get middle-low 6 bits
        out.push_back(static_cast<char>(0b10000000 | (cp & mask)));          // get lower 6 bits
    }

    return out;
}
// NOLINTEND

static void decode_html_entities(std::string& str) {
    std::string result = str;
    size_t pos = 0;

    // https://en.wikipedia.org/wiki/List_of_XML_and_HTML_character_entity_references#Character_reference_overview
    // HTML entities are encoded as &#x{hex}; or &#{decimal}; where the number is a Unicode code point
    while ((pos = result.find("&#", pos)) != std::string::npos) {
        size_t end = result.find(';', pos);
        if (end == std::string::npos) {  // malformed entity, skip it
            pos += 2;
            continue;
        }
        // Get the entity
        std::string entity = result.substr(pos + 2, end - pos - 2);

        // Parse the entity
        uint32_t value = 0;  // store the Unicode code point
        if (!entity.empty() && (entity[0] == 'x' || entity[0] == 'X')) {
            value =
                std::stoul(entity.substr(1), nullptr,
                           16);  // parse hex NOLINT(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
        } else {
            value = std::stoul(entity);  // parse decimal
        }

        result.replace(pos, end - pos + 1, append_utf8(value));
        pos++;
    }

    str = std::move(result);
}

std::vector<WikiEntry> fetch_wiki_stats() {
    std::vector<WikiEntry> wiki_stats;

    // Fetch CSV data
    cpr::Response r =
        cpr::Get(cpr::Url{"https://wikistats.wmcloud.org/api.php?action=dump&table=wikipedias&format=csv"});

    // Parse CSV
    std::istringstream stream(r.text);
    std::string line;
    bool first_line = true;

    while (std::getline(stream, line)) {
        if (first_line) {
            first_line = false;  // Skip header
            continue;
        }

        if (line.empty()) continue;

        auto fields = split_csv_line(line);
        if (fields.size() >= 37) {  // Need at least 37 fields to access fields[36]
                                    // NOLINT(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
            WikiEntry stats;
            try {
                std::string en_lang_name = fields[1];
                std::string local_lang_name =
                    fields[10];  // NOLINT(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

                // Decode HTML entities in language names
                decode_html_entities(en_lang_name);
                decode_html_entities(local_lang_name);

                // Sometimes the language name is wrapped in quotes, so we need to remove them
                en_lang_name.erase(remove(en_lang_name.begin(), en_lang_name.end(), '\"'), en_lang_name.end());
                local_lang_name.erase(remove(local_lang_name.begin(), local_lang_name.end(), '\"'),
                                      local_lang_name.end());

                stats.language_code = fields[2];     // prefix
                stats.language_name = en_lang_name;  // lang
                stats.local_language_name = local_lang_name;
                stats.wiki_id = fields[36];  // NOLINT(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
                stats.articles = std::stoi(fields[4]);  // good
                stats.users = std::stoi(
                    fields[7]);  // users NOLINT(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
                stats.is_downloaded = false;
                if (!stats.language_code.empty()) {
                    wiki_stats.push_back(stats);
                }
            } catch (const std::exception&) {
                // Skip malformed lines
                continue;
            }
        }
    }

    // Sort fetched wikisby number of users.
    // The number of users is a good proxy for the size of the wiki.
    std::ranges::sort(wiki_stats, [](const WikiEntry& a, const WikiEntry& b) { return a.users > b.users; });

    return wiki_stats;
}
