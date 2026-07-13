add_library(Log)

target_sources(Log
  PRIVATE
    FileLog.cpp
  PUBLIC
    FILE_SET HEADERS
)
target_compile_features(Log PUBLIC cxx_std_23)
set_target_properties(Log PROPERTIES
  CXX_EXTENSIONS OFF
)
target_link_libraries(Log
  PUBLIC
    Utils
  PRIVATE
    spdlog::spdlog
)
