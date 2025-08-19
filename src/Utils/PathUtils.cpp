#include "PathUtils.h"

#include <filesystem>

namespace PathUtils {

std::filesystem::path get_data_dir() {
    std::filesystem::path current_dir = std::filesystem::current_path();

    // Check if we're running from the build directory (relative to project root)
    if (std::filesystem::exists(current_dir / "data")) {
        return current_dir / "data";
    }

    // Check if we're running from the project root
    if (std::filesystem::exists(current_dir / "../data")) {
        return current_dir / "../data";
    }

    // Default: create data directory in current working directory
    return current_dir / "data";
}

std::filesystem::path get_logs_dir() {
    std::filesystem::path current_dir = std::filesystem::current_path();

    // Check if we're running from the build directory (relative to project root)
    if (std::filesystem::exists(current_dir / "logs")) {
        return current_dir / "logs";
    }

    // Check if we're running from the project root
    if (std::filesystem::exists(current_dir / "../logs")) {
        return current_dir / "../logs";
    }

    // Default: create logs directory in current working directory
    return current_dir / "logs";
}

void ensure_data_dir_exists() { std::filesystem::create_directories(get_data_dir()); }

void ensure_logs_dir_exists() { std::filesystem::create_directories(get_logs_dir()); }
}  // namespace PathUtils
