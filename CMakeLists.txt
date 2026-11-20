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

# The target CPU: x86_64 or arm64. Under MSVC, the compiler's: in a Developer PowerShell set up to build for ARM64 on
# an x64 machine, CMAKE_SYSTEM_PROCESSOR is the build machine's.
if(MSVC)
  string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" WG_ARCH)
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" WG_ARCH)
endif()
string(REGEX REPLACE "^(amd64|x64)$" "x86_64" WG_ARCH "${WG_ARCH}")
string(REGEX REPLACE "^aarch64$" "arm64" WG_ARCH "${WG_ARCH}")

# MSVC reads source files in the system's code page unless told otherwise; they are UTF-8
add_compile_options("$<$<COMPILE_LANG_AND_ID:C,MSVC>:/utf-8>" "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>")

# The dependencies are linked into the executable (cpr would otherwise build shared libraries along with its curl)
option(BUILD_SHARED_LIBS "Build the dependencies as shared libraries" OFF)
option(WIKIGRAPH_STATIC "One static executable for every Linux distribution (cmake/StaticLinux.cmake)" OFF)

# FetchContent clones the dependencies with Git, which Visual Studio installs but leaves off the PATH of its
# Developer PowerShell
if(CMAKE_HOST_WIN32 AND DEFINED ENV{VSINSTALLDIR})
  find_program(GIT_EXECUTABLE git
    PATHS "$ENV{VSINSTALLDIR}Common7/IDE/CommonExtensions/Microsoft/TeamFoundation/Team Explorer/Git/cmd")
endif()
include(FetchContent)

if(WIKIGRAPH_STATIC)
  include(cmake/StaticLinux.cmake)
endif()

# Terminal UI Library
FetchContent_Declare(ftxui
  GIT_REPOSITORY    "https://github.com/ArthurSonzogni/ftxui"
  GIT_TAG           "v7.0.3"
  SYSTEM
)
FetchContent_MakeAvailable(ftxui)

# zlib streams the gzip-compressed dumps (the graph is built from them once, then cached)
find_package(ZLIB REQUIRED)

# rapidgzip decompresses the dumps on several threads, in memory. OFF: zlib on one thread (about 4 times slower)
option(WIKIGRAPH_PARALLEL_GZIP "Decompress the dumps on several threads with rapidgzip" ON)
if(WIKIGRAPH_PARALLEL_GZIP)
  include(cmake/Isal.cmake)
  if(WIKIGRAPH_ISAL)
    set(WG_RAPIDGZIP_SUBMODULES "src/external/isa-l")
  endif()
  FetchContent_Declare(librapidarchive
    GIT_REPOSITORY    "https://github.com/mxmlnkn/librapidarchive"
    GIT_TAG           "rapidgzip-v0.16.2"
    GIT_SUBMODULES    "${WG_RAPIDGZIP_SUBMODULES}"  # ISA-L's, if used: its bundled zlib and rpmalloc are not
    SYSTEM
  )
  set(LIBRAPIDARCHIVE_BUILD_TESTS OFF)
  set(LIBRAPIDARCHIVE_BUILD_TOOLS OFF)
  set(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB ON)
  set(LIBRAPIDARCHIVE_WITH_ISAL ${WIKIGRAPH_ISAL})
  set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF)
  # It forces the macOS deployment target down to 13.0 in the cache; keep ours
  set(WG_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  FetchContent_MakeAvailable(librapidarchive)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${WG_OSX_DEPLOYMENT_TARGET}" CACHE STRING "Minimum macOS version" FORCE)
  if(TARGET isal)  # its C files get rapidgzip's long list of warnings
    target_compile_options(isal PRIVATE "$<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<C_COMPILER_ID:MSVC>>>:-w>")
  endif()
endif()

# libcurl wrapper for downloading wikipedia dumps
FetchContent_Declare(cpr
  GIT_REPOSITORY    "https://github.com/libcpr/cpr"
  GIT_TAG           "1.15.1"
  SYSTEM
)
# The system's libcurl (building curl with libpsl needs meson), unless set otherwise, as the static build does
if(NOT DEFINED CPR_USE_SYSTEM_CURL)
  set(CPR_USE_SYSTEM_CURL ON)
endif()
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
elseif(MSVC)
  target_compile_definitions(wikigraph_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)  # getenv is not "unsafe" here
endif()

add_library(wikigraph_core STATIC ${CORE_SOURCES})
target_include_directories(wikigraph_core PUBLIC src)
target_link_libraries(wikigraph_core PUBLIC ZLIB::ZLIB PRIVATE wikigraph_warnings)
if(WIKIGRAPH_PARALLEL_GZIP)
  target_link_libraries(wikigraph_core PRIVATE rapidgzip::rapidgzip)
  target_compile_definitions(wikigraph_core PRIVATE WG_PARALLEL_GZIP)
  # rapidgzip's lookup tables are computed at compile time
  target_compile_options(wikigraph_core PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/bigobj;/constexpr:steps199000100>")
endif()
# std::atomic of the progress structs is not lock-free; GCC/Clang on Linux implement it in libatomic
if(UNIX AND NOT APPLE)
  target_link_libraries(wikigraph_core PUBLIC atomic)
endif()

add_library(wikigraph_fetch STATIC ${FETCH_SOURCES})
# curl's headers include windows.h, which otherwise defines min and max as macros
target_compile_definitions(wikigraph_fetch PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX>)
target_link_libraries(wikigraph_fetch PUBLIC wikigraph_core cpr::cpr spdlog::spdlog PRIVATE wikigraph_warnings)

add_executable(wikigraph ${APP_SOURCES})
target_link_libraries(wikigraph PRIVATE wikigraph_fetch ftxui::component wikigraph_warnings)  # fetch brings the core
target_sources(wikigraph PRIVATE ${WG_STATIC_SOURCES})  # from cmake/StaticLinux.cmake

# Unit and integration tests of the core (tests/). Off by default; the CI enables it.
option(WIKIGRAPH_TESTS "Build the tests (fetches Catch2)" OFF)
if(WIKIGRAPH_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
