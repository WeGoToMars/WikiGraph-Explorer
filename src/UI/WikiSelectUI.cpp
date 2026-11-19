#include "WikiSelectUI.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FetchWikiData/DownloadWikiDump.h"
#include "FetchWikiData/FetchWikiStats.h"
#include "UI.h"
#include "UI/UIBase.h"
#include "Utils/PathUtils.h"

using namespace ftxui;

namespace wg::app {

namespace {

// ------------------------------------------------------------------------------------------------ downloaded wikis

/// The parts of a dump file name, "<db>wiki-<YYYYMMDD>-<type>.sql.gz".
struct DumpFileName {
    std::string db, date;
    WikiFileType type;
};

std::optional<DumpFileName> parse_dump_file_name(std::string_view name) {
    constexpr std::string_view kWiki = "wiki-", kSuffix = ".sql.gz";
    constexpr size_t kDateLength = std::string_view("YYYYMMDD").size();
    const size_t wiki = name.find(kWiki);
    if (!name.ends_with(kSuffix) || wiki == std::string_view::npos) return std::nullopt;
    const size_t date = wiki + kWiki.size();
    const size_t type = date + kDateLength + 1;  // after the '-'
    const size_t type_end = name.size() - kSuffix.size();
    if (type > type_end) return std::nullopt;
    const std::string_view token = name.substr(type, type_end - type);
    const auto file_type = wiki_file_type(token);
    if (!file_type) {
        spdlog::error("Unknown file type: {} in file: {}", token, name);
        return std::nullopt;
    }
    return DumpFileName{.db = std::string(name.substr(0, wiki)),
                        .date = std::string(name.substr(date, kDateLength)),
                        .type = *file_type};
}

/// The wikis in the data folder whose page, linktarget and pagelinks dumps are all present.
std::vector<DownloadedWiki> scan_downloaded_wikis() {
    const std::filesystem::path& data_dir = paths::data_dir();
    spdlog::info("Scanning for downloaded wikis in directory: {}", data_dir.string());
    std::vector<DownloadedWiki> found;
    try {
        if (!std::filesystem::exists(data_dir)) {
            spdlog::warn("Data directory does not exist: {}", data_dir.string());
            return found;
        }
        std::map<std::pair<std::string, std::string>, DownloadedWiki> by_db_and_date;
        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            const auto parsed = parse_dump_file_name(filename);
            if (!parsed) continue;
            spdlog::debug("Found wiki file: {} ({} bytes)", filename, entry.file_size());
            DownloadedWiki& wiki = by_db_and_date[{parsed->db, parsed->date}];
            wiki.language_code = parsed->db;
            wiki.date = parsed->date;
            wiki.file(parsed->type) = WikiFile{.exists = true, .data_path = entry.path()};
        }
        for (const auto& wiki : by_db_and_date | std::views::values) {
            spdlog::info("{} wiki: {} {}", wiki.complete() ? "Complete" : "Incomplete", wiki.language_code, wiki.date);
            if (wiki.complete()) found.push_back(wiki);
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        spdlog::error("Error getting downloaded wikis: {}", ex.what());
    }
    return found;
}

}  // namespace

void WikiListRows::rebuild(const std::vector<WikiEntry>& stats, bool list_loading) {
    names_.clear();
    rows_.clear();

    // Downloaded wikis are named after their files, which use the database prefix ("zh_min_nan", not "zh-min-nan")
    std::map<std::string, const WikiEntry*> by_db_prefix;
    for (const auto& stat : stats) by_db_prefix[dump_db_prefix(stat.language_code)] = &stat;

    downloaded_ = scan_downloaded_wikis();

    // First the downloaded wikis
    for (const auto& wiki : downloaded_) {
        WikiEntry row;
        row.language_code = wiki.language_code;
        if (const auto it = by_db_prefix.find(wiki.language_code); it != by_db_prefix.end()) {
            row.language_name = it->second->language_name + " (" + wiki.date + ")";
            row.local_language_name = it->second->local_language_name;
        } else {
            row.language_name = std::format("{}wiki ({})", wiki.language_code, wiki.date);
        }
        rows_.push_back(row);
    }
    // Then every wiki that can be downloaded (including newer dumps of the downloaded ones)
    rows_.insert(rows_.end(), stats.begin(), stats.end());

    if (rows_.empty()) {  // neither downloaded nor online: one row that says so
        WikiEntry placeholder;
        placeholder.language_name = list_loading ? "Loading..." : "No wikis available";
        rows_.push_back(placeholder);
    }
    for (const auto& row : rows_) names_.push_back(row.language_name);
}

namespace {

// ------------------------------------------------------------------------------------------------ events

/// Enter: open the selected wiki if it is downloaded, otherwise download it.
void open_selected(UIState& state, const WikiListRows& rows) {
    const auto index = static_cast<size_t>(state.wikis.selected_index);
    const WikiEntry& entry = rows.row(index);
    const bool downloaded = rows.is_downloaded(index);
    spdlog::debug("Selected wiki index: {}, language_code: '{}', downloaded: {}", index, entry.language_code,
                  downloaded);
    if (entry.language_code.empty()) return;  // the "No wikis available" placeholder
    if (downloaded) {
        // Downloaded wikis are named after their files, which already use the database prefix
        const DownloadedWiki& wiki = rows.downloaded(index);
        state.selected = SelectedWiki{.prefix = wiki.language_code, .date = wiki.date, .wiki = wiki};
        start_preparing_graph(state);
    } else {
        state.selected.prefix = dump_db_prefix(entry.language_code);
        start_download(state);
    }
}

/// The mouse wheel moves the selection.
bool scroll_list(const Mouse& mouse, UIState& state, const WikiListRows& rows) {
    int step = 0;
    if (mouse.button == Mouse::WheelUp)
        step = -1;
    else if (mouse.button == Mouse::WheelDown)
        step = 1;
    else
        return false;
    int& selected = state.wikis.selected_index;
    const int moved = std::clamp(selected + step, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    if (moved != selected) {
        selected = moved;
        post_ui_refresh();
    }
    return true;
}

// `event` by value: Event::mouse() is not const
bool handle_list_event(Event event, UIState& state, const WikiListRows& rows) {
    if (event.character() == "q") {
        if (auto* screen = ScreenInteractive::Active()) screen->Exit();
        return true;
    }
    if (event.is_mouse()) return scroll_list(event.mouse(), state, rows);
    if (event == Event::Return && rows.size() != 0) {
        open_selected(state, rows);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ drawing

/// Pick up the list fetched in the background, and rescan the data folder when asked to.
void refresh_rows(UIState& state, WikiListRows& rows) {
    WikiListState& wikis = state.wikis;
    if (wikis.fetched_ready.exchange(false)) {  // the background fetch has finished
        wikis.entries = std::move(wikis.fetched);
        wikis.loading = false;
        wikis.offline = wikis.entries.empty();
        wikis.rescan = true;
    }
    if (wikis.rescan) {  // also when back from a wiki: show what has been downloaded since
        wikis.rescan = false;
        rows.rebuild(wikis.entries, wikis.loading);
        wikis.selected_index = std::clamp(wikis.selected_index, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    }
}

/// Column widths, shared by the header and the rows so they line up.
constexpr std::array<int, 5> kColumnWidths = {25, 20, 12, 10, 12};

Element cell(Element e, size_t column) { return std::move(e) | size(WIDTH, EQUAL, kColumnWidths[column]); }

Element render_header() {
    auto header =
        Table({{cell(text("Language (en)") | bold, 0), cell(text("Language (local)") | bold, 1),
                cell(text("Code") | bold, 2), cell(text("Articles") | bold, 3), cell(text("Users") | bold, 4)}});
    header.SelectAll().SeparatorVertical(LIGHT);
    return header.Render();
}

Element render_rows(const WikiListRows& rows, int selected) {
    std::vector<std::vector<Element>> cells;
    for (size_t i = 0; i < rows.size(); ++i) {
        const WikiEntry& row = rows.row(i);
        const bool stats = !rows.is_downloaded(i);  // no statistics for the downloaded wikis
        cells.push_back({cell(paragraph(row.language_name), 0), cell(paragraph(row.local_language_name), 1),
                         cell(text(row.language_code), 2),
                         cell(text(stats ? std::format("{:L}", row.articles) : "") | align_right, 3),
                         cell(text(stats ? std::format("{:L}", row.users) : "") | align_right, 4)});
    }
    auto table = Table(cells);
    table.SelectAll().SeparatorVertical(LIGHT);
    for (size_t i = 0; i < rows.size(); ++i) {
        auto line = table.SelectRow(static_cast<int>(i));
        if (static_cast<int>(i) == selected)
            line.Decorate(bgcolor(Color::Blue) | color(Color::White) | focus);
        else if (rows.is_downloaded(i))
            line.Decorate(bgcolor(Color::GreenLight) | color(Color::Black));
    }
    return table.Render();
}

Element render_wiki_list(const UIState& state, const WikiListRows& rows) {
    const int selected = state.wikis.selected_index;
    Element status = text("");
    if (state.wikis.loading)
        status = text(" (loading the list of Wikipedias...)") | color(Color::Yellow);
    else if (state.wikis.offline)
        status = text(" (offline mode)") | color(Color::Red1);
    return vbox({hbox({text("Select Wikipedia Language") | bold, status}), separator(), render_header(), separator(),
                 render_rows(rows, selected) | vscroll_indicator | frame | flex, separator(),
                 text(std::format("[{}/{}]: Enter to select, q or Esc to quit", selected + 1, rows.size())) |
                     color(Color::GrayDark)}) |
           border;
}

}  // namespace

Component create_wiki_select_ui(UIState& state) {
    auto rows = std::make_shared<WikiListRows>();
    rows->rebuild(state.wikis.entries, state.wikis.loading);

    // The menu is not drawn: it tracks the selection for the table below
    auto menu = Menu(&rows->names(), &state.wikis.selected_index);
    auto menu_with_events =
        CatchEvent(menu, [&state, rows](const Event& event) { return handle_list_event(event, state, *rows); });
    return Renderer(menu_with_events, [&state, rows] {
        refresh_rows(state, *rows);
        return render_wiki_list(state, *rows);
    });
}

}  // namespace wg::app
