#pragma once
#include <filesystem>

namespace PathUtils {
/**
 * @brief Get the data directory path relative to the working directory
 * @return Path to the data directory
 */
std::filesystem::path get_data_dir();

/**
 * @brief Get the logs directory path relative to the working directory
 * @return Path to the logs directory
 */
std::filesystem::path get_logs_dir();

/**
 * @brief Ensure the data directory exists, creating it if necessary
 */
void ensure_data_dir_exists();

/**
 * @brief Ensure the logs directory exists, creating it if necessary
 */
void ensure_logs_dir_exists();
}  // namespace PathUtils
