cmake_minimum_required(VERSION 3.28)

project(wikigraph)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set minimum macOS version to 13.3 for std::to_chars support
if(APPLE)
  add_compile_options(-mmacosx-version-min=13.3)
  add_link_options(-mmacosx-version-min=13.3)
endif()

# Optional custom hashmap intergration
option(USE_STD_UNORDERED_MAP "Use std::unordered_map instead of emhash")
set(USE_STD_UNORDERED_MAP OFF CACHE BOOL "Use std::unordered_map instead of emhash" FORCE)
# Use this approach to avoid rebuilding the whole project, only the files that include Hashmap.h will be recompiled
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/Utils/Hashmap.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/Utils/Hashmap.h)

# Parallel decompression and loading of wikipedia dumps
option(PARALLEL_DECOMPRESSION "Use parallel decompression of wikipedia dumps" OFF)
set(PARALLEL_DECOMPRESSION OFF CACHE BOOL "Use parallel decompression of wikipedia dumps" FORCE)

# Terminal UI Library
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY    "https://github.com/ArthurSonzogni/ftxui"
  GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)

# Optional rapidgzip integration
if(PARALLEL_DECOMPRESSION)
  FetchContent_Declare(rapidgzip
    GIT_REPOSITORY    "https://github.com/mxmlnkn/rapidgzip"
    GIT_TAG           "5b1384efd9e32155bd9ae2bf08d3cc50d9709caf" # pin the commit to avoid breaking changes
  )
  
  # Configure rapidgzip
  set(LIBRAPIDARCHIVE_BUILD_TESTS OFF CACHE BOOL "Disable tests" FORCE)
  set(LIBRAPIDARCHIVE_BUILD_TOOLS OFF CACHE BOOL "Disable tools" FORCE)
  set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF CACHE BOOL "Disable rpmalloc" FORCE)
  set(LIBRAPIDARCHIVE_WITH_ISAL OFF CACHE BOOL "Disable isal" FORCE)
  
  FetchContent_MakeAvailable(rapidgzip)
  # rapidzip uses a lot of constexpr, so we need to increase the limit
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fconstexpr-steps=20000000)
  endif()

  message(STATUS "Using rapidgzip for parallel decompression")

  # High-performance concurrent queue for thread coordination
  FetchContent_Declare(concurrentqueue
  GIT_REPOSITORY    "https://github.com/cameron314/concurrentqueue"
  GIT_TAG           "v1.0.4"
  )
  FetchContent_MakeAvailable(concurrentqueue)
else()
  # Require zlib >= 1.2.9 for gzoffset
  find_package(ZLIB 1.2.9 REQUIRED)
  message(STATUS "Using system zlib")
endif()

# libcurl wrapper for downloading wikipedia dumps
FetchContent_Declare(cpr
  GIT_REPOSITORY    "https://github.com/libcpr/cpr"
  GIT_TAG           "1.12.0"
)
FetchContent_MakeAvailable(cpr)
set(CPR_FORCE_USE_SYSTEM_CURL ON) # To build curl libpsl is required, which requires mason to be installed

if(NOT USE_STD_UNORDERED_MAP)
  # A faster hashmap implementation than std::unordered_map for faster loading
  FetchContent_Declare(emhash
    GIT_REPOSITORY    "https://github.com/ktprime/emhash"
    GIT_TAG           "master"
    SOURCE_SUBDIR     "HEADER ONLY" # https://discourse.cmake.org/t/prevent-fetchcontent-makeavailable-to-execute-cmakelists-txt/12704/3
  )
  FetchContent_MakeAvailable(emhash)
  message(STATUS "emhash_SOURCE_DIR: ${emhash_SOURCE_DIR}")
endif()

# Logging library
FetchContent_Declare(spdlog
  GIT_REPOSITORY    "https://github.com/gabime/spdlog"
  GIT_TAG           "v1.15.3"
)
FetchContent_MakeAvailable(spdlog)

# Add all sources to the project
file(GLOB_RECURSE SOURCES src/*.cpp)
add_executable(wikigraph ${SOURCES})

target_include_directories(wikigraph PRIVATE src)

# Mark dependencies as system to avoid compiler warnings
target_include_directories(wikigraph SYSTEM PRIVATE ${ftxui_SOURCE_DIR}/include)
target_include_directories(wikigraph SYSTEM PRIVATE ${cpr_SOURCE_DIR}/include)
target_include_directories(wikigraph SYSTEM PRIVATE ${spdlog_SOURCE_DIR}/include)

if(PARALLEL_DECOMPRESSION)
  target_include_directories(wikigraph SYSTEM PRIVATE ${rapidgzip_SOURCE_DIR}/src)
  target_include_directories(wikigraph SYSTEM PRIVATE ${concurrentqueue_SOURCE_DIR})
endif()

if(NOT USE_STD_UNORDERED_MAP)
  target_include_directories(wikigraph SYSTEM PRIVATE ${emhash_SOURCE_DIR})
endif()

# Link rapidgzip if enabled
if(PARALLEL_DECOMPRESSION)
  target_link_libraries(wikigraph PRIVATE librapidgzip)
else()
  target_link_libraries(wikigraph PRIVATE ZLIB::ZLIB)
endif()

target_link_libraries(wikigraph PRIVATE
    ftxui::component
    ftxui::screen
    cpr::cpr
    spdlog::spdlog
)

if(USE_STD_UNORDERED_MAP)
  message(STATUS "Using std::unordered_map instead of emhash")
  target_compile_definitions(wikigraph PRIVATE USE_STD_UNORDERED_MAP)
else()
  message(STATUS "Using emhash instead of std::unordered_map")
endif()

if(PARALLEL_DECOMPRESSION)
  message(STATUS "Using rapidgzip for decompression")
  target_compile_definitions(wikigraph PRIVATE PARALLEL_DECOMPRESSION)
else()
  message(STATUS "Using standard zlib for decompression")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Enable all warnings for project's code
  # https://clang.llvm.org/docs/DiagnosticsReference.html
  target_compile_options(wikigraph PRIVATE 
    -Weverything 
    -Wno-system-headers        # Ignore warnings from dependencies (marked as system)
    -Wno-c++98-compat          # Ignore warnings from C++98 compatibility
    -Wno-c++98-compat-pedantic 
    -Wno-sign-conversion       # Ignore warnings about unsigned to signed conversions
    -Wno-padded                # Ignore warnings about padding in structs
    -Wno-shorten-64-to-32      # Ignore warnings about 64-bit to 32-bit conversions
    -Wno-switch-default        # Handled by the clang-tidy
  )
endif()

include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
endif()