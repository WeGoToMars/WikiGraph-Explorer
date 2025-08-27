#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Forward declarations
struct Page;
struct DownloadURLs;
struct WikiEntry;
class PageLoader;

#include <ftxui/component/screen_interactive.hpp>

struct ReadProgress {
    uint64_t total_bytes;    // Total compressed file size in bytes
    uint64_t current_bytes;  // Current position in compressed file in bytes
};

// UI stage enum
enum class UIStage : uint8_t {
    WikiSelection,
    Download,
    LoadPages,
    LoadLinkTargets,
    LoadLinks,
    BuildingGraph,
    UserInput,
    ShowPaths,
    Done
};

// The type of file in the wikipedia dump
enum class WikiFileType : uint8_t { Page, LinkTarget, PageLinks };

/**
 * @brief Mapping from file type string tokens to `WikiFileType` enum values.
 */
static std::map<std::string, WikiFileType> wiki_file_type_map() {
    return {
        {"page", WikiFileType::Page},
        {"linktarget", WikiFileType::LinkTarget},
        {"pagelinks", WikiFileType::PageLinks},
    };
}

struct WikiFile {
    bool exists = false;

    std::string lang_code;
    std::string date;
    WikiFileType file_type;

    uint64_t file_size;

    std::filesystem::path data_path;
    std::filesystem::path index_path;
};

/// @brief Structure representing a downloaded wiki with its metadata
struct DownloadedWiki {
    std::string language_code;
    std::string date;
    uint64_t size_on_disk;

    WikiFile page;
    WikiFile linktarget;
    WikiFile pagelinks;
};

// The main UI state struct
struct UIState {
    bool offline_mode = false;

    // Progress counters
    std::atomic<size_t> page_count{0};
    std::atomic<uint32_t> page_speed{0};
    std::atomic<ReadProgress> page_progress{ReadProgress{.total_bytes = 0, .current_bytes = 0}};
    std::atomic<size_t> linktarget_count{0};
    std::atomic<uint32_t> linktarget_speed{0};
    std::atomic<ReadProgress> linktarget_progress{ReadProgress{.total_bytes = 0, .current_bytes = 0}};
    std::atomic<size_t> link_count{0};
    std::atomic<uint32_t> link_speed{0};
    std::atomic<ReadProgress> link_progress{ReadProgress{.total_bytes = 0, .current_bytes = 0}};

    // Graph build progress counters
    using GraphBuildProgress = struct {
        uint64_t processed_links;  // number of edges inserted into adjacency list
        uint64_t total_links;      // total number of edges to insert
        uint32_t edges_speed;      // edges inserted per second
    };
    std::atomic<GraphBuildProgress> graph_build_progress{GraphBuildProgress{0, 0, 0}};

    // BFS progress counters
    using bfs_progress_counter = struct {
        uint32_t current_layer;         // current layer of BFS
        uint32_t layer_size;            // number of nodes in current layer
        uint32_t layer_explored_count;  // number of nodes explored in current layer (for layer progress bar)
        uint32_t total_explored_nodes;  // total number of nodes explored (for total progress bar)
    };
    std::atomic<bfs_progress_counter> bfs_progress{bfs_progress_counter{0, 0, 0, 0}};
    std::atomic<bool> is_searching{false};

    // Timing of the different stages of the program (for benchmarking)
    std::chrono::milliseconds page_load_duration{0};
    std::chrono::milliseconds linktarget_load_duration{0};
    std::chrono::milliseconds link_load_duration{0};
    std::chrono::milliseconds graph_build_duration{0};

    // Current stage
    std::atomic<UIStage> stage{UIStage::WikiSelection};

    // User input and results
    std::string selected_wiki_prefix;
    std::string selected_wiki_date;
    DownloadedWiki selected_wiki;

    // Access to loaders
    PageLoader* page_loader{nullptr};

    // User input
    int selected_wiki_index{0};
    std::string start_title;
    std::string end_title;

    // UI display
    std::string error_message;
    std::vector<std::vector<uint32_t>> found_paths;
    std::chrono::milliseconds search_duration{0};

    // Download progress pair
    using DownloadProgress = struct {
        uint64_t dlnow;
        uint64_t dltotal;
        uint64_t dlspeed;
    };

    // Download state
    std::atomic<DownloadProgress> page_download_progress{DownloadProgress{0, 0, 0}};
    std::atomic<bool> page_download_complete{false};
    std::atomic<DownloadProgress> pagelinks_download_progress{DownloadProgress{0, 0, 0}};
    std::atomic<bool> pagelinks_download_complete{false};
    std::atomic<DownloadProgress> linktarget_download_progress{DownloadProgress{0, 0, 0}};
    std::atomic<bool> linktarget_download_complete{false};
    std::string download_error_message;

    // Referesh rate
    static constexpr std::chrono::milliseconds refresh_rate{200};
};

/// @brief Request a manual UI refresh by posting a custom event to the active screen.
[[maybe_unused]] static void post_ui_refresh() {
    auto* screen = ftxui::ScreenInteractive::Active();
    if (screen != nullptr) {
        screen->PostEvent(ftxui::Event::Custom);
    }
}
