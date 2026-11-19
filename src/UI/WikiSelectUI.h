#pragma once

#include <ftxui/component/component_base.hpp>
#include <string>
#include <vector>

#include "FetchWikiData/FetchWikiStats.h"
#include "UI/UIBase.h"

namespace wg::app {

/// Rows of the wiki selection screen: downloaded wikis first, then every wiki listed by WikiStats.
class WikiListRows {
   public:
    /// Rebuild the rows: rescans the data folder for downloaded wikis
    void rebuild(const std::vector<WikiEntry>& stats, bool list_loading);

    /// The row labels, for the menu that tracks the selection
    [[nodiscard]] const std::vector<std::string>& names() const { return names_; }
    [[nodiscard]] size_t size() const { return rows_.size(); }
    [[nodiscard]] const WikiEntry& row(size_t index) const { return rows_[index]; }
    /// Downloaded wikis are the first rows
    [[nodiscard]] bool is_downloaded(size_t index) const { return index < downloaded_.size(); }
    [[nodiscard]] const DownloadedWiki& downloaded(size_t index) const { return downloaded_[index]; }

   private:
    std::vector<std::string> names_;
    std::vector<WikiEntry> rows_;
    std::vector<DownloadedWiki> downloaded_;
};

/**
 * Build the wiki selection screen component. Enter opens a downloaded wiki or downloads the selected one.
 * @param state Shared UI state; the list of wikis is state.wikis
 */
ftxui::Component create_wiki_select_ui(UIState& state);

}  // namespace wg::app
