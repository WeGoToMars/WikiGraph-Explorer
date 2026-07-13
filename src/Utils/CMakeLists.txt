# Generate Hashmap.h from the CMake option so only consumers of Hashmap.h rebuild
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/Hashmap.h.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/Hashmap.h"
)

add_library(Utils)

target_sources(Utils
  PRIVATE
    PathUtils.cpp
  PUBLIC
    FILE_SET HEADERS
)
target_compile_features(Utils PUBLIC cxx_std_23)
set_target_properties(Utils PROPERTIES
  CXX_EXTENSIONS OFF
)

if(NOT USE_STD_UNORDERED_MAP)
  target_link_libraries(Utils PUBLIC emhash::emhash)
endif()

if(PARALLEL_DECOMPRESSION)
  target_link_libraries(Utils PUBLIC concurrentqueue spdlog::spdlog)
endif()
