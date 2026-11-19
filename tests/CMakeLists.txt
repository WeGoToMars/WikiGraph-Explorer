# Unit and integration tests for the core. They link wikigraph_core only, so they also check that the core builds
# without the terminal UI and the downloader. Run with: ctest --test-dir build/<preset> --output-on-failure

FetchContent_Declare(Catch2
  GIT_REPOSITORY    "https://github.com/catchorg/Catch2"
  GIT_TAG           "v3.8.1"
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(wikigraph_tests ${TEST_SOURCES})
target_link_libraries(wikigraph_tests PRIVATE wikigraph_core Catch2::Catch2WithMain)

# The CI's synthetic wiki (tools/ci/make_test_wiki.py), written once before the tests that read it
set(WG_TEST_WIKI_DIR "${CMAKE_CURRENT_BINARY_DIR}/testwiki")
target_compile_definitions(wikigraph_tests PRIVATE WG_TEST_WIKI_DIR="${WG_TEST_WIKI_DIR}")
add_test(NAME make_test_wiki
         COMMAND Python3::Interpreter -I "${PROJECT_SOURCE_DIR}/tools/ci/make_test_wiki.py" "${WG_TEST_WIKI_DIR}")
set_tests_properties(make_test_wiki PROPERTIES FIXTURES_SETUP test_wiki)

catch_discover_tests(wikigraph_tests PROPERTIES FIXTURES_REQUIRED test_wiki)
