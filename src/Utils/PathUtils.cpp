#include "PathUtils.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "Graph/Platform.h"

namespace wg::paths {

namespace {

std::filesystem::path g_data_dir;
std::filesystem::path g_logs_dir;
bool g_initialized = false;

bool can_create(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec && std::filesystem::is_directory(dir, ec);
}

}  // namespace

void init(const std::filesystem::path& data_dir_override) {
    if (g_initialized) return;
    g_initialized = true;

    std::filesystem::path chosen = data_dir_override;
    if (chosen.empty()) {
        const char* env = std::getenv("WIKIGRAPH_DATA_DIR");  // NOLINT(concurrency-mt-unsafe): startup only
        if (env != nullptr && *env != '\0') chosen = env;
    }
    if (!chosen.empty()) {
        g_data_dir = std::filesystem::absolute(chosen).lexically_normal();
        g_logs_dir = g_data_dir / "logs";
        can_create(g_data_dir);
        can_create(g_logs_dir);
        return;
    }

    const std::filesystem::path exe = wg::executable_path();
    if (!exe.empty()) {
        const std::filesystem::path root = (exe.parent_path() / ".." / "resources").lexically_normal();
        if (can_create(root / "data") && can_create(root / "logs")) {
            g_data_dir = root / "data";
            g_logs_dir = root / "logs";
            return;
        }
    }
    std::filesystem::path root = wg::user_data_dir("WikiGraph-Explorer");
    if (root.empty()) root = std::filesystem::current_path() / "wikigraph-data";
    g_data_dir = root / "data";
    g_logs_dir = root / "logs";
    can_create(g_data_dir);
    can_create(g_logs_dir);
}

const std::filesystem::path& data_dir() {
    init();
    return g_data_dir;
}

const std::filesystem::path& logs_dir() {
    init();
    return g_logs_dir;
}

}  // namespace wg::paths
