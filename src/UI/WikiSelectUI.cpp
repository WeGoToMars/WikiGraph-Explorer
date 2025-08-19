#include "WikiSelectUI.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <map>

#include "UI.h"
#include "Utils/PathUtils.h"

using namespace ftxui;

std::vector<DownloadedWiki> WikiSelectUIManager::scan_downloaded_wikis() {
    const std::filesystem::path data_dir = PathUtils::get_data_dir();

    spdlog::info("Scanning for downloaded wikis in directory: {}", data_dir.string());

    try {
        if (!std::filesystem::exists(data_dir)) {
            spdlog::warn("Data directory does not exist: {}", data_dir.string());
            return downloaded_wikis;
        }

        // Map to track which files exist for each wiki and their sizes
        // wiki_files[lang_code,date] = DownloadedWiki
        std::map<std::pair<std::string, std::string>, DownloadedWiki> wiki_files;

        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            // Check if it matches the pattern: {lang}wiki-{date}-{type}.sql.gz
            if (entry.is_regular_file() && entry.path().string().ends_with(".sql.gz")) {
                std::string filename = entry.path().filename().string();

                size_t wiki_pos = filename.find("wiki-");
                if (wiki_pos == std::string::npos) continue;

                // Extract language code
                std::string lang_code = filename.substr(0, wiki_pos);

                // Extract date (YYYYMMDD)
                constexpr size_t date_string_length = std::string("YYYYMMDD").size();
                constexpr size_t wiki_pos_offset = std::string("wiki-").size();
                std::string date = filename.substr(wiki_pos + wiki_pos_offset, date_string_length);

                // Extract file type
                constexpr size_t file_type_offset = std::string("wiki-YYYYMMDD-").size();
                std::string file_type = filename.substr(wiki_pos + file_type_offset,
                                                        filename.find(".sql.gz") - wiki_pos - file_type_offset);

                WikiFile wiki_file;
                wiki_file.exists = true;
                wiki_file.lang_code = lang_code;
                wiki_file.date = date;

                if (wiki_file_type_map().contains(file_type)) {
                    wiki_file.file_type = wiki_file_type_map().at(file_type);
                } else {
                    spdlog::error("Unknown file type: {} in file: {}", file_type, filename);
                    continue;
                }

                wiki_file.file_size = entry.file_size();
                wiki_file.data_path = entry.path();

                spdlog::debug(
                    "Found wiki file: {} (lang: {}, date: {}, type: {}, "
                    "size: {} bytes)",
                    filename, lang_code, date, file_type, entry.file_size());

                // Sequential loader does not require an index anymore
                switch (wiki_file.file_type) {
                    case WikiFileType::Page:
                        wiki_files[{lang_code, date}].page = wiki_file;
                        break;
                    case WikiFileType::LinkTarget:
                        wiki_files[{lang_code, date}].linktarget = wiki_file;
                        break;
                    case WikiFileType::PageLinks:
                        wiki_files[{lang_code, date}].pagelinks = wiki_file;
                        break;
                }
            }
        }

        // Check which wikis have all required files (page, linktarget, pagelinks)
        spdlog::info("Checking {} wiki groups for complete downloads", wiki_files.size());

        for (auto& [key, wiki] : wiki_files) {
            if (wiki.page.exists && wiki.linktarget.exists && wiki.pagelinks.exists) {
                wiki.language_code = key.first;
                wiki.date = key.second;
                wiki.size_on_disk = wiki.page.file_size + wiki.linktarget.file_size + wiki.pagelinks.file_size;
                downloaded_wikis.push_back(wiki);

                spdlog::info("Found complete wiki: {} {}", wiki.language_code, wiki.date);
            } else {
                spdlog::info("Incomplete wiki: {} {}", wiki.language_code, wiki.date);
            }
        }
    } catch (const std::filesystem::filesystem_error& ex) {
        spdlog::error("Error getting downloaded wikis: {}", ex.what());
    }

    return downloaded_wikis;
}

void WikiSelectUIManager::initialize(UIState& state, const std::vector<WikiEntry>& stats) {
    // Clear all data to ensure fresh state
    wiki_names.clear();
    static_stats.clear();
    stats_map.clear();
    is_downloaded.clear();
    downloaded_wikis.clear();

    if (!state.offline_mode) {
        for (const auto& stat : stats) {
            stats_map[stat.language_code] = stat;
        }
    }

    // Get list of already downloaded wikis
    downloaded_wikis = this->scan_downloaded_wikis();

    // First, add downloaded wikis to the top of the list
    for (const auto& wiki : downloaded_wikis) {
        WikiEntry downloaded_stat;
        downloaded_stat.language_code = wiki.language_code;

        if (!state.offline_mode) {
            downloaded_stat.language_name = stats_map[wiki.language_code].language_name + " (" + wiki.date + ")";
            downloaded_stat.local_language_name = stats_map[wiki.language_code].local_language_name;
        } else {
            downloaded_stat.language_name = std::format("{}wiki ({})", wiki.language_code, wiki.date);
        }
        downloaded_stat.is_downloaded = true;

        static_stats.push_back(downloaded_stat);
        wiki_names.push_back(downloaded_stat.language_name);
        is_downloaded.push_back(true);
    }

    // Then add online wikis that aren't already downloaded
    for (const auto& stat : stats) {
        static_stats.push_back(stat);
        wiki_names.push_back(stat.language_name);
        is_downloaded.push_back(false);
    }

    if (wiki_names.empty()) {
        // If no wikis at all (neither downloaded nor online), show a message
        wiki_names.push_back("No wikis available");
        WikiEntry empty_stat;
        empty_stat.language_name = "No wikis available";
        static_stats.push_back(empty_stat);
        is_downloaded.push_back(false);
    }
}

Component create_wiki_select_ui(UIState& state, std::vector<WikiEntry>& entries,
                                std::function<void()> on_wiki_selected) {
    // Create UI manager instance
    auto ui_manager = std::make_shared<WikiSelectUIManager>();
    ui_manager->initialize(state, entries);

    // Create menu component (invisible, just for navigation)
    auto menu = Menu(&ui_manager->get_wiki_names(), &state.selected_wiki_index);

    // Add event handling
    auto menu_with_events = CatchEvent(menu, [&state, ui_manager, &on_wiki_selected](const Event& event) {
        auto* screen = ScreenInteractive::Active();
        if (event.character() == "q") {
            if (screen) {
                screen->Exit();
            }
            return true;
        }
        if (event == Event::Return && !ui_manager->get_static_stats().empty()) {
            const auto& selected_stat = ui_manager->get_stat_at(state.selected_wiki_index);
            spdlog::debug("Selected wiki index: {}, language_code: '{}', is_downloaded: {}", state.selected_wiki_index,
                          selected_stat.language_code, selected_stat.is_downloaded);
            state.selected_wiki_prefix = selected_stat.language_code;
            if (selected_stat.is_downloaded) {
                state.selected_wiki = ui_manager->get_downloaded_wikis()[state.selected_wiki_index];
                state.stage = UIStage::LoadPages;
                post_ui_refresh();

                // Call the callback if provided
                if (on_wiki_selected) {
                    on_wiki_selected();
                }
            } else {
                state.stage = UIStage::Download;
                post_ui_refresh();
            }
            return true;
        }
        return false;
    });

    // Create a custom renderer that displays the menu as a table
    auto table_menu = Renderer(menu_with_events, [ui_manager, &state] {
        // Calculate visible window for scrolling
        const int visible_rows = 12;  // Number of data rows visible at once
        const int total_rows = static_cast<int>(ui_manager->get_static_stats().size());
        const int selected = state.selected_wiki_index;

        // Calculate scroll offset to keep selected item visible
        static int scroll_offset = 0;
        if (selected < scroll_offset) {
            scroll_offset = selected;
        } else if (selected >= scroll_offset + visible_rows) {
            scroll_offset = selected - visible_rows + 1;
        }

        // Ensure scroll_offset is within bounds
        scroll_offset = std::max(0, std::min(scroll_offset, total_rows - visible_rows));
        if (total_rows <= visible_rows) scroll_offset = 0;

        // Create table data with only visible rows
        std::vector<std::vector<std::string>> table_data;

        // Header row
        table_data.push_back({"Language (en)", "Language (local)", "Code", "Articles", "Users"});

        // Add visible data rows
        int end_row = std::min(scroll_offset + visible_rows, total_rows);
        for (int i = scroll_offset; i < end_row; i++) {
            const auto& stat = ui_manager->get_stat_at(i);

            std::vector<std::string> row = {std::format("{:<26}", stat.language_name),
                                            std::format("{:<20}", stat.local_language_name),
                                            std::format("{:<12}", stat.language_code),
                                            stat.is_downloaded ? "" : std::format("{:>10L}", stat.articles),
                                            stat.is_downloaded ? "" : std::format("{:>12L}", stat.users)};
            table_data.push_back(row);
        }

        // Create table
        auto table = Table(table_data);
        table.SelectAll().Border(LIGHT);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).SeparatorVertical(LIGHT);
        table.SelectRow(0).Border(DOUBLE);

        // Highlight selected row and downloaded rows
        for (int i = scroll_offset; i < end_row; i++) {
            int visible_row = i - scroll_offset + 1;  // +1 for header

            if (i == selected) {
                // Selected row - blue background (highest priority)
                table.SelectRow(visible_row).Decorate(bgcolor(Color::Blue));
                table.SelectRow(visible_row).Decorate(color(Color::White));
            } else if (ui_manager->is_wiki_downloaded(i)) {
                // Downloaded row - green background (only if not selected)
                table.SelectRow(visible_row).Decorate(bgcolor(Color::GreenLight));
                table.SelectRow(visible_row).Decorate(color(Color::Black));
            }
        }

        // Create scroll indicator info
        std::string scroll_info;
        if (total_rows > visible_rows) {
            scroll_info = std::format(" (Showing {}-{} of {})", scroll_offset + 1,
                                      std::min(scroll_offset + visible_rows, total_rows), total_rows);
        }

        std::string offline_mode_warning;
        if (state.offline_mode) {
            offline_mode_warning = " (offline mode)";
        }

        return vbox({hbox({text("Select Wikipedia Language" + scroll_info) | bold,
                           text(offline_mode_warning) | color(Color::Red1)}),
                     separator(), table.Render() | frame, separator(),
                     text("Use arrow "
                          "keys to navigate, Enter to select, 'q' "
                          "to quit") |
                         color(Color::GrayDark)}) |
               border;
    });

    return table_menu;
}
