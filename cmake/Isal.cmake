# WIKIGRAPH_ISAL: rapidgzip decodes with ISA-L's assembly inflate, about 2.5 times faster where it knows where a
# chunk starts (the second pagelinks pass). On by default where it builds: on x86-64 with NASM, and on arm64 except
# with MSVC, which cannot assemble its GNU-syntax code. rapidgzip does not support it on macOS.

set(WG_ISAL_DEFAULT OFF)
if(NOT APPLE AND WG_ARCH STREQUAL "x86_64")
  include(CheckLanguage)
  check_language(ASM_NASM)
  if(CMAKE_ASM_NASM_COMPILER)
    set(WG_ISAL_DEFAULT ON)
  else()
    message(WARNING "NASM not found: building without ISA-L, so the second pagelinks pass is slower")
  endif()
elseif(NOT APPLE AND WG_ARCH STREQUAL "arm64" AND NOT MSVC)
  set(WG_ISAL_DEFAULT ON)
endif()
option(WIKIGRAPH_ISAL "Decompress with ISA-L's assembly inflate where rapidgzip can" ${WG_ISAL_DEFAULT})
message(STATUS "rapidgzip: on, ISA-L: ${WIKIGRAPH_ISAL}")
