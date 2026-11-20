#pragma once

#include <filesystem>

/// Where dumps, graphs and logs are stored. The rule does not depend on the working directory:
///   1. the folder given to init() (the --data-dir option),
///   2. the WIKIGRAPH_DATA_DIR environment variable,
///   3. resources/data next to the build folder: <folder of the executable>/../resources/data, where the
///      CMake presets put it (build/release/wikigraph -> build/resources/data),
///   4. the per-user data folder (see wg::user_data_dir) if 3. cannot be created, e.g. for an installed binary.
/// With 1. or 2., logs go to <data folder>/logs; otherwise to resources/logs next to the data folder.
namespace wg::paths {

/// Resolve the folders and create them. Call once at startup; later calls do nothing.
void init(const std::filesystem::path& data_dir_override = {});

/// Folder holding the dumps and the graphs built from them.
const std::filesystem::path& data_dir();

/// Folder holding log.txt.
const std::filesystem::path& logs_dir();

}  // namespace wg::paths
