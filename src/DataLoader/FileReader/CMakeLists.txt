add_library(FileReader)

target_sources(FileReader
  PRIVATE
    AsyncLineReader.cpp
    ParallelLineReader.cpp
    SQLParserUtils.cpp
  PUBLIC
    FILE_SET HEADERS
)
target_compile_features(FileReader PUBLIC cxx_std_23)
set_target_properties(FileReader PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(FileReader
  PUBLIC
    UIBase
  PRIVATE
    spdlog::spdlog
)

if(PARALLEL_DECOMPRESSION)
  target_link_libraries(FileReader
    PUBLIC
      concurrentqueue
    PRIVATE
      librapidgzip
  )
else()
  target_link_libraries(FileReader PUBLIC ZLIB::ZLIB)
endif()
