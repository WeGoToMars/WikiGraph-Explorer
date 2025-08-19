#include <locale>

#include "DataLoader/DataLoaderManager.h"
#include "FetchWikiData/DownloadWikiDump.h"
#include "Log/FileLog.h"
#include "UI/UI.h"
#include "Utils/PathUtils.h"

int main() {
    init_logfile();

    PathUtils::ensure_data_dir_exists();

    // Speed up I/O operations by disabling synchronization with the C standard library
    std::ios_base::sync_with_stdio(false);

    // Set locale to UTF-8 to ensure proper handling of Unicode characters
    std::locale::global(std::locale("en_US.UTF-8"));

    // Create UI state object
    UIState state;

    // Start with wiki selection stage (default stage)
    state.stage = UIStage::WikiSelection;

    // Create data loader manager object
    auto data_manager = std::make_unique<DataLoaderManager>();
    // Expose page loader for UI searches
    state.page_loader = &data_manager->get_page_loader();

    // Create callback function that will start the data loader thread when a wiki is selected
    auto on_wiki_selected = [&state, &data_manager]() { start_loader_thread(state, data_manager); };

    // Run the UI with the callback (blocks until user exits)
    run_ui(state, on_wiki_selected);

    return 0;
}
