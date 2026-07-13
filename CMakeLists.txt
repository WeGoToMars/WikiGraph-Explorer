cmake_minimum_required(VERSION 3.28)

project(wikigraph LANGUAGES CXX C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional custom hashmap integration
option(USE_STD_UNORDERED_MAP "Use std::unordered_map instead of emhash" OFF)

# Parallel decompression and loading of wikipedia dumps
option(PARALLEL_DECOMPRESSION "Use parallel decompression of wikipedia dumps" OFF)

# Set minimum macOS version to 13.3 for std::to_chars support
if(APPLE)
  add_compile_options(-mmacosx-version-min=13.3)
  add_link_options(-mmacosx-version-min=13.3)
endif()

include(FetchContent)

# Terminal UI Library
FetchContent_Declare(ftxui
  GIT_REPOSITORY "https://github.com/ArthurSonzogni/ftxui"
  GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)

# Optional rapidgzip integration
if(PARALLEL_DECOMPRESSION)
  # Honor these as normal vars in the dependency's option() calls (CMP0077)
  set(ISAL_BUILD_SHARED_LIBS OFF)
  set(LIBRAPIDARCHIVE_BUILD_TESTS OFF)
  set(LIBRAPIDARCHIVE_BUILD_TOOLS OFF)
  set(LIBRAPIDARCHIVE_WITH_RPMALLOC ON)
  set(LIBRAPIDARCHIVE_WITH_ISAL ON)

  set(_wikigraph_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)

  FetchContent_Declare(rapidgzip
    GIT_REPOSITORY "https://github.com/mxmlnkn/rapidgzip"
    GIT_TAG "5b1384efd9e32155bd9ae2bf08d3cc50d9709caf" # pin the commit to avoid breaking changes
  )
  FetchContent_MakeAvailable(rapidgzip)

  set(BUILD_SHARED_LIBS "${_wikigraph_build_shared_libs}")
  unset(_wikigraph_build_shared_libs)

  # rapidgzip uses constexpr to precompute LUTs, so we need to increase the limit
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(librapidgzip PRIVATE -fconstexpr-steps=20000000)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(librapidgzip PRIVATE /constexpr:steps 20000000)
  endif()

  message(STATUS "Using rapidgzip for parallel decompression")

  # High-performance concurrent queue for thread coordination
  FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY "https://github.com/cameron314/concurrentqueue"
    GIT_TAG "v1.0.4"
  )
  FetchContent_MakeAvailable(concurrentqueue)
else()
  # Require zlib >= 1.2.9 for gzoffset
  find_package(ZLIB 1.2.9 REQUIRED)
  message(STATUS "Using system zlib for decompression")
endif()

# libcurl wrapper for downloading wikipedia dumps
# Prefer system curl to avoid building libpsl (requires meson)
set(CPR_USE_SYSTEM_CURL ON)
FetchContent_Declare(cpr
  GIT_REPOSITORY "https://github.com/libcpr/cpr"
  GIT_TAG "1.12.0"
)
FetchContent_MakeAvailable(cpr)

if(NOT USE_STD_UNORDERED_MAP)
  # Faster hashmap than std::unordered_map for loading
  FetchContent_Declare(emhash
    GIT_REPOSITORY "https://github.com/ktprime/emhash"
    GIT_TAG "master"
    SOURCE_SUBDIR "HEADER ONLY" # skip emhash's CMakeLists.txt
  )
  FetchContent_MakeAvailable(emhash)

  add_library(emhash INTERFACE)
  add_library(emhash::emhash ALIAS emhash)
  # Headers live under include/emhash/; Hashmap.h includes <hash_tableN.hpp>
  target_include_directories(emhash SYSTEM INTERFACE "${emhash_SOURCE_DIR}/include/emhash")
  message(STATUS "emhash_SOURCE_DIR: ${emhash_SOURCE_DIR}")
endif()

# Logging library
FetchContent_Declare(spdlog
  GIT_REPOSITORY "https://github.com/gabime/spdlog"
  GIT_TAG "v1.15.3"
)
FetchContent_MakeAvailable(spdlog)

add_subdirectory(src)

# Point clangd (and other tools) at whichever preset was just configured
file(CREATE_LINK
  "${CMAKE_BINARY_DIR}/compile_commands.json"
  "${CMAKE_SOURCE_DIR}/compile_commands.json"
  SYMBOLIC
)

include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
  set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
endif()
