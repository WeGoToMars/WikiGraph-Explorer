#pragma once
#include <filesystem>

namespace PathUtils {
/**
 * @brief Get the resource directory path relative to the working directory
 * @param folder_name The name of the folder to get the path for
 * @return Path to the resource directory
 */
std::filesystem::path get_resource_dir(std::string folder_name);

/**
 * @brief Ensure the data directory exists, creating it if necessary
 */
void ensure_data_dir_exists();

/**
 * @brief Ensure the logs directory exists, creating it if necessary
 */
void ensure_logs_dir_exists();
}  // namespace PathUtils
