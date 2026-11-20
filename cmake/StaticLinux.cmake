# WIKIGRAPH_STATIC: one executable that runs on any Linux distribution, linked statically against musl, curl and
# OpenSSL. Build it with a musl toolchain such as Alpine's (tools/ci/build-static-linux.sh): glibc does not support
# static linking well. Sets WG_STATIC_SOURCES, which the executable adds.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "WIKIGRAPH_STATIC is for Linux. On Windows, the vcpkg-msvc presets link statically.")
endif()

# musl gives new threads 128 KiB of stack; the size in the executable's header raises that to glibc's 8 MiB
add_link_options(-static -Wl,-z,stack-size=8388608 "$<$<CONFIG:Release>:-s>")
set(ZLIB_USE_STATIC_LIBS ON)
set(OPENSSL_USE_STATIC_LIBS ON)

# cpr builds curl, with OpenSSL and without libpsl (public suffixes only matter for cookies, which are not used). The
# CA certificates are looked up at run time (src/FetchWikiData/Http.h).
set(CPR_USE_SYSTEM_CURL OFF)
set(CPR_FORCE_OPENSSL_BACKEND ON)
set(CPR_CURL_USE_LIBPSL OFF)

# musl's malloc is compact but slow when many threads allocate at once, as the graph build does: mimalloc, linked in
# as an object file, replaces it
FetchContent_Declare(mimalloc
  GIT_REPOSITORY    "https://github.com/microsoft/mimalloc"
  GIT_TAG           "v3.7.0"
  SYSTEM
)
set(MI_OVERRIDE ON)
set(MI_BUILD_SHARED OFF)
set(MI_BUILD_STATIC ON)  # not linked, but the object-file target depends on it
set(MI_BUILD_OBJECT ON)
set(MI_BUILD_TESTS OFF)
set(MI_INSTALL_TOPLEVEL OFF)
FetchContent_MakeAvailable(mimalloc)
set(WG_STATIC_SOURCES $<TARGET_OBJECTS:mimalloc-obj>)

# musl's memcpy, memchr and memcmp are portable C, a few bytes per step
if(WG_ARCH STREQUAL "x86_64")
  # FreeBSD's assembly (third_party/freebsd-libc/README.md). ASM is enabled here, before rapidgzip enables NASM:
  # CMake cannot set it up after ASM_NASM.
  enable_language(ASM)
  set(wg_libc ${PROJECT_SOURCE_DIR}/third_party/freebsd-libc)
  set(wg_string_sources ${wg_libc}/memchr.S ${wg_libc}/memcmp.S ${wg_libc}/memcpy.S)
  set_source_files_properties(${wg_string_sources} PROPERTIES INCLUDE_DIRECTORIES ${wg_libc}/include)
else()
  # 16-byte vector versions in C, compiled without LTO, the builtins and the loop-to-call pattern matching (which
  # could turn their loops into calls to themselves)
  set(wg_string_sources ${PROJECT_SOURCE_DIR}/src/Static/StringFunctions.c)
  set_source_files_properties(${wg_string_sources} PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns;-fno-lto")
endif()
list(APPEND WG_STATIC_SOURCES ${wg_string_sources})
