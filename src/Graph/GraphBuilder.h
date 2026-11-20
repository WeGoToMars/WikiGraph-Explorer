#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace wg {

/// The dump files of one wiki. `redirect` may be empty for downloads made before it was required.
struct DumpFiles {
    std::filesystem::path page, linktarget, pagelinks, redirect;
};

enum class BuildStep : uint8_t { Pages, Redirects, LinkTargets, LinksCount, LinksFill, Finish };
inline constexpr int kBuildSteps = 6;
const char* build_step_name(BuildStep step);

struct BuildProgress {
    BuildStep step = BuildStep::Pages;
    uint64_t done = 0;   ///< compressed bytes read (or sub-steps done in Finish)
    uint64_t total = 0;  ///< compressed file size (or sub-step count)
    uint64_t rows = 0;   ///< rows parsed so far in this step
};

struct BuildReport {
    uint64_t pages = 0, articles = 0, redirects = 0, redirects_unresolved = 0;
    uint64_t pagelinks_rows = 0, edges = 0, duplicate_links = 0;
    uint64_t list_bytes = 0;
    double seconds = 0;
    uint64_t peak_footprint = 0;
};

/// Thrown by build_graph / ensure_graph after a stop request. The partial build is removed.
struct BuildCancelled : std::runtime_error {
    BuildCancelled() : std::runtime_error("graph build cancelled") {}
    ~BuildCancelled() override;  // defined in GraphBuilder.cpp, so the vtable is emitted once
};

/// Where the graph for a set of dumps lives: data/<lang>wiki-<date>.graph next to the page dump.
std::filesystem::path graph_dir_for(const DumpFiles& dumps);

/// True if `dir` holds a graph of the current format built from exactly these dump files.
bool graph_is_current(const std::filesystem::path& dir, const DumpFiles& dumps);

/// Build the graph from the dumps and write it to `dir` (via a temporary directory, renamed at the end).
///
/// Streams each dump once (pagelinks twice: degrees, then fill), so peak memory is about 8 bytes per link plus the
/// page tables. Throws std::runtime_error on failure, BuildCancelled after a stop request (checked between chunks
/// and phases; the longest step without a check is a few seconds for English Wikipedia).
BuildReport build_graph(const DumpFiles& dumps, const std::filesystem::path& dir, unsigned threads,
                        const std::function<void(const BuildProgress&)>& progress, std::stop_token stop = {});

/**
 * Build the graph for `dumps` unless an up-to-date one already exists (or `force` is set).
 * @return true if a build ran; `report` receives its figures
 */
bool ensure_graph(const DumpFiles& dumps, bool force, unsigned threads,
                  const std::function<void(const BuildProgress&)>& progress, BuildReport* report,
                  std::stop_token stop = {});

}  // namespace wg
