# Header-only target for shared UI types (breaks cycles with DataLoader/FileReader/etc.)
add_library(UIBase INTERFACE)

target_sources(UIBase
  PUBLIC
    FILE_SET HEADERS
)
target_link_libraries(UIBase
  INTERFACE
    ftxui::component
    ftxui::screen
)

add_library(UI)

target_sources(UI
  PRIVATE
    UI.cpp
    WikiSelectUI.cpp
  PUBLIC
    FILE_SET HEADERS
)
# Higher-level deps (DataLoader, FetchWikiData, PageGraph) are linked from src/CMakeLists.txt
# after those targets exist.
target_compile_features(UI PUBLIC cxx_std_23)
set_target_properties(UI PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(UI
  PUBLIC
    UIBase
    Utils
    ftxui::component
    ftxui::screen
  PRIVATE
    spdlog::spdlog
)
