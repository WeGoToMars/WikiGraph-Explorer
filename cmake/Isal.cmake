# WIKIGRAPH_ISAL: rapidgzip decodes with ISA-L's assembly inflate, about 2.5 times faster where it knows where a
# chunk starts (the second pagelinks pass). On by default where it builds: on x86-64 with NASM, and on arm64 except
# with MSVC, which cannot assemble its GNU-syntax code. rapidgzip does not support it on macOS.

set(WG_ISAL_DEFAULT OFF)
if(NOT APPLE AND WG_ARCH STREQUAL "x86_64")
  include(CheckLanguage)
  check_language(ASM_NASM)
  # Windows has no package manager to install it from: download the portable NASM that vcpkg uses (same file,
  # mirrors and hash as vcpkg's scripts/cmake/vcpkg_find_acquire_program(NASM).cmake)
  if(NOT CMAKE_ASM_NASM_COMPILER AND CMAKE_HOST_WIN32)
    FetchContent_Declare(nasm
      URL "https://www.nasm.us/pub/nasm/releasebuilds/3.01/win64/nasm-3.01-win64.zip"
          "https://vcpkg.github.io/assets/nasm/nasm-3.01-win64.zip"
      URL_HASH SHA512=771c238ddb17c98d5736ccaba4ade1d1601d896f09e588489cb43a4f6381bc0ae14d1869f5316fe94f847f54867e65cf12665529b1e7ad88e5e7d3e162719a4f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR "no-cmake-project"  # only the executable is needed
    )
    FetchContent_MakeAvailable(nasm)
    set(CMAKE_ASM_NASM_COMPILER "${nasm_SOURCE_DIR}/nasm.exe" CACHE FILEPATH "NASM, for ISA-L" FORCE)
    unset(CMAKE_ASM_NASM_COMPILER)  # check_language() also left a NOTFOUND variable that hides the cache entry
  endif()
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
