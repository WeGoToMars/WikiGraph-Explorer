#include "PathUtils.h"

#include <filesystem>

namespace PathUtils {

std::filesystem::path get_resource_dir(std::string folder_name) {
    std::filesystem::path current_dir = std::filesystem::current_path();

    // Check if that folder exists in the current working directory
    if (std::filesystem::exists(current_dir / folder_name)) {
        return current_dir / folder_name;
    }

    // Check if that folder exists in the parent directory
    if (std::filesystem::exists(current_dir / ("../" + folder_name))) {
        return current_dir / ("../" + folder_name);
    }

    // Default: create resources directory in current working directory
    return current_dir / "../resources" / folder_name;
}

void ensure_data_dir_exists() { std::filesystem::create_directories(get_resource_dir("data")); }

void ensure_logs_dir_exists() { std::filesystem::create_directories(get_resource_dir("logs")); }
}  // namespace PathUtils
