if(PARALLEL_DECOMPRESSION)
  add_compile_definitions(PARALLEL_DECOMPRESSION)
endif()

if(USE_STD_UNORDERED_MAP)
  add_compile_definitions(USE_STD_UNORDERED_MAP)
  message(STATUS "Using std::unordered_map instead of emhash")
else()
  message(STATUS "Using emhash instead of std::unordered_map")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # https://clang.llvm.org/docs/DiagnosticsReference.html
  add_compile_options(
    -Weverything
    -Wno-system-headers
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-sign-conversion
    -Wno-padded
    -Wno-shorten-64-to-32
    -Wno-exit-time-destructors
    -Wno-global-constructors
    -Wno-switch-default
  )
endif()

# Order matters: UIBase (via UI/) before FetchWikiData/DataLoader; Utils before Log/UI.
add_subdirectory(Utils)
add_subdirectory(Log)
add_subdirectory(UI)
add_subdirectory(FetchWikiData)
add_subdirectory(DataLoader)
add_subdirectory(PageGraph)

# Resolve the DataLoader <-> PageGraph static-library cycle once both targets exist.
target_link_libraries(DataLoader PUBLIC PageGraph)
target_link_libraries(PageGraph PUBLIC DataLoader)

# UI depends on the higher-level modules.
target_link_libraries(UI PUBLIC DataLoader FetchWikiData PageGraph)

add_executable(wikigraph)
target_sources(wikigraph PRIVATE main.cpp)
target_compile_features(wikigraph PRIVATE cxx_std_23)
set_target_properties(wikigraph PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(wikigraph
  PRIVATE
    DataLoader
    FetchWikiData
    Log
    PageGraph
    UI
    Utils
)
