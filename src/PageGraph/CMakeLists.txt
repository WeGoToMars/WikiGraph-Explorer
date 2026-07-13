add_library(PageGraph)

target_sources(PageGraph
  PRIVATE
    PageGraph.cpp
  PUBLIC
    FILE_SET HEADERS
)
# DataLoader is linked from src/CMakeLists.txt after both targets exist (static-lib cycle).
target_compile_features(PageGraph PUBLIC cxx_std_23)
set_target_properties(PageGraph PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(PageGraph
  PUBLIC
    UIBase
  PRIVATE
    spdlog::spdlog
)
