add_subdirectory(FileReader)

add_library(DataLoader)

target_sources(DataLoader
  PRIVATE
    DataLoaderManager.cpp
    LinkLoader.cpp
    LinkTargetLoader.cpp
    PageLoader.cpp
  PUBLIC
    FILE_SET HEADERS
)
# PageGraph is linked from src/CMakeLists.txt after both targets exist (static-lib cycle).
target_compile_features(DataLoader PUBLIC cxx_std_23)
set_target_properties(DataLoader PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(DataLoader
  PUBLIC
    FileReader
    Utils
    UIBase
  PRIVATE
    spdlog::spdlog
)
