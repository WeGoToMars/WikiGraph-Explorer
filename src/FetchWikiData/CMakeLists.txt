add_library(FetchWikiData)

target_sources(FetchWikiData
  PRIVATE
    DownloadWikiDump.cpp
    FetchWikiStats.cpp
  PUBLIC
    FILE_SET HEADERS
)
target_compile_features(FetchWikiData PUBLIC cxx_std_23)
set_target_properties(FetchWikiData PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(FetchWikiData
  PUBLIC
    UIBase
  PRIVATE
    cpr::cpr
    spdlog::spdlog
)
