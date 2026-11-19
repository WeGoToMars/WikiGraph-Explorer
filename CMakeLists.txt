cmake_minimum_required(VERSION 3.28)

# Minimum macOS version: 13.3 for std::to_chars. Must be set before project(); applies to the dependencies too.
# A lower value (cached by older build trees, or set on the command line) is raised to 13.3.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 13.3)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(wikigraph)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Terminal UI Library
include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY    "https://github.com/ArthurSonzogni/ftxui"
  GIT_TAG           "v7.0.3"
  SYSTEM
)
FetchContent_MakeAvailable(ftxui)

# zlib streams the gzip-compressed dumps (the graph is built from them once, then cached)
find_package(ZLIB REQUIRED)

# libcurl wrapper for downloading wikipedia dumps
FetchContent_Declare(cpr
  GIT_REPOSITORY    "https://github.com/libcpr/cpr"
  GIT_TAG           "1.15.1"
  SYSTEM
)
# Use the system's libcurl: building curl needs libpsl, which needs meson. Must be set before cpr is configured.
set(CPR_USE_SYSTEM_CURL ON)
FetchContent_MakeAvailable(cpr)

# Logging library
FetchContent_Declare(spdlog
  GIT_REPOSITORY    "https://github.com/gabime/spdlog"
  GIT_TAG           "v1.17.0"
  SYSTEM
)
FetchContent_MakeAvailable(spdlog)

# Three targets, so the layering is enforced by the build: the core cannot include FTXUI or cpr headers.
#   wikigraph_core   dump reader, graph builder and store, search, paths (zlib only; logs through wg::log)
#   wikigraph_fetch  WikiStats list, dump lookup, downloads and checksums (cpr)
#   wikigraph        terminal UI and command line (FTXUI), spdlog log file
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/Graph/*.cpp src/Utils/*.cpp)
file(GLOB_RECURSE FETCH_SOURCES CONFIGURE_DEPENDS src/FetchWikiData/*.cpp)
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/UI/*.cpp src/Log/*.cpp src/main.cpp)

# Warnings for the project's own code only
add_library(wikigraph_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Enable all warnings for project's code
  # https://clang.llvm.org/docs/DiagnosticsReference.html
  target_compile_options(wikigraph_warnings INTERFACE
    -Weverything
    -Wno-system-headers        # Ignore warnings from dependencies (marked as system)
    -Wno-c++98-compat          # Ignore warnings from C++98 compatibility
    -Wno-c++98-compat-pedantic
    -Wno-sign-conversion       # Ignore warnings about unsigned to signed conversions
    -Wno-padded                # Ignore warnings about padding in structs
    -Wno-shorten-64-to-32      # Ignore warnings about 64-bit to 32-bit conversions
    -Wno-exit-time-destructors # Ignore warnings about objects that need to persist until the end of the program
    -Wno-global-constructors   # ditto
    -Wno-switch-default        # Handled by the clang-tidy
    -Wno-thread-safety-negative # Would need every mutex-taking function annotated with REQUIRES(!mutex)
  )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(wikigraph_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_library(wikigraph_core STATIC ${CORE_SOURCES})
target_include_directories(wikigraph_core PUBLIC src)
target_link_libraries(wikigraph_core PUBLIC ZLIB::ZLIB PRIVATE wikigraph_warnings)
# std::atomic of the progress structs is not lock-free; GCC/Clang on Linux implement it in libatomic
if(UNIX AND NOT APPLE)
  target_link_libraries(wikigraph_core PUBLIC atomic)
endif()

add_library(wikigraph_fetch STATIC ${FETCH_SOURCES})
target_link_libraries(wikigraph_fetch PUBLIC wikigraph_core cpr::cpr spdlog::spdlog PRIVATE wikigraph_warnings)

add_executable(wikigraph ${APP_SOURCES})
target_link_libraries(wikigraph PRIVATE wikigraph_fetch ftxui::component wikigraph_warnings)  # fetch brings the core
