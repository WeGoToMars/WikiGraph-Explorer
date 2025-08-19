#pragma once

#include <ftxui/component/component_base.hpp>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "FetchWikiData/FetchWikiStats.h"
#include "UI/UIBase.h"

using ftxui::Component;

// Forward declaration
struct UIState;

/// @brief Class to manage wiki selection UI state and data
class WikiSelectUIManager {
   private:
    std::vector<std::string> wiki_names;
    std::vector<WikiEntry> static_stats;
    std::map<std::string, WikiEntry> stats_map;
    std::vector<bool> is_downloaded;
    std::vector<DownloadedWiki> downloaded_wikis;

    /// @brief Scan the data directory for already downloaded Wikipedia files.
    std::vector<DownloadedWiki> scan_downloaded_wikis();

   public:
    /// @brief Initialize the UI manager with fresh data
    void initialize(UIState& state, const std::vector<WikiEntry>& stats);

    /// @brief Get the current wiki names
    const std::vector<std::string>& get_wiki_names() const {
        return wiki_names;
    }

    /// @brief Get the current stats
    const std::vector<WikiEntry>& get_static_stats() const {
        return static_stats;
    }

    /// @brief Get the downloaded status
    const std::vector<bool>& get_is_downloaded() const {
        return is_downloaded;
    }

    /// @brief Get the downloaded wikis
    const std::vector<DownloadedWiki>& get_downloaded_wikis() const {
        return downloaded_wikis;
    }

    /// @brief Get a specific stat by index
    [[nodiscard]] const WikiEntry& get_stat_at(size_t index) const {
        return static_stats[index];
    }

    /// @brief Check if a wiki at index is downloaded
    bool is_wiki_downloaded(size_t index) const {
        return index < is_downloaded.size() && is_downloaded[index];
    }
};

/**
 * @brief Build the wiki selection screen component.
 * @param state Shared UI state to update based on user actions
 * @param stats Mutable list of available wikis with stats
 * @param on_wiki_selected Optional callback invoked after a selection is made
 * @return ftxui Component for the selection UI
 */
Component create_wiki_select_ui(UIState& state, std::vector<WikiEntry>& stats,
                                std::function<void()> on_wiki_selected = nullptr);
