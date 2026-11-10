# =============================================================================
# Boris - CMake build
# =============================================================================
#
# This is the build for Windows (MSVC or clang-cl). On Linux and macOS use
# the Makefile; this file works there too, but the Makefile is the reference.
#
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
#     cmake --build build --config Release
#     ctest --test-dir build -C Release --output-on-failure
#
# The only dependency is libcurl:   vcpkg install curl:x64-windows
#
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# With a static vcpkg triplet (x64-windows-static) link the C runtime
# statically too: boris.exe then needs no DLLs at all.
if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(boris C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(CURL REQUIRED)

# The same file sets the Makefile builds: everything in src/ and src/tools/.
file(GLOB BORIS_SOURCES CONFIGURE_DEPENDS src/*.c src/tools/*.c)
list(FILTER BORIS_SOURCES EXCLUDE REGEX "/src/(main|http_client)\.c$")
file(GLOB BORIS_TESTS CONFIGURE_DEPENDS tests/test_*.c)

if(MSVC)
  set(BORIS_WARNINGS
    /W4 /WX
    /wd4100   # unused parameter (callbacks have fixed signatures)
    /wd4200   # flexible array member: standard since C99
    /wd4204   # non-constant aggregate initialiser: standard since C99
    /wd4206   # empty translation unit: the POSIX-only files, on Windows
    /wd4221   # initialiser takes the address of a local: likewise
    /wd4244   # int conversions that -Wall -Wextra do not flag either
    /wd4267   # size_t -> int, as above
    /wd4706   # assignment within a condition
  )
  set(BORIS_DEFINITIONS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  # Source files are UTF-8, and so are the strings compiled from them.
  set(BORIS_OPTIONS /utf-8)
else()
  set(BORIS_WARNINGS -Wall -Wextra -Werror)
  set(BORIS_DEFINITIONS _DEFAULT_SOURCE)
  set(BORIS_OPTIONS)
endif()

# Everything except main() and the HTTP transport, which test_e2e swaps for
# tests/mock_http.c at link time.
add_library(boris_core STATIC ${BORIS_SOURCES} vendor/cJSON.c)
target_include_directories(boris_core PUBLIC include vendor)
target_compile_options(boris_core PRIVATE ${BORIS_WARNINGS} ${BORIS_OPTIONS})
target_compile_definitions(boris_core PUBLIC ${BORIS_DEFINITIONS})
target_link_libraries(boris_core PUBLIC CURL::libcurl)
if(WIN32)
  target_link_libraries(boris_core PUBLIC crypt32)   # DPAPI, for the stored API key
endif()

add_library(boris_http STATIC src/http_client.c)
target_compile_options(boris_http PRIVATE ${BORIS_WARNINGS} ${BORIS_OPTIONS})
target_link_libraries(boris_http PUBLIC boris_core)
if(WIN32)
  target_link_libraries(boris_http PUBLIC ws2_32)
endif()

# Settings every executable needs on Windows.
function(boris_executable target)
  target_compile_options(${target} PRIVATE ${BORIS_WARNINGS} ${BORIS_OPTIONS})
  if(WIN32)
    # UTF-8 code page and long paths (see the manifest).
    target_sources(${target} PRIVATE windows/boris.manifest)
    if(MSVC)
      target_link_options(${target} PRIVATE
        # fopen() defaults to binary mode, so "\n" stays "\n" on disk.
        binmode.obj
        # The same 8 MB stack as Linux; path buffers are PATH_MAX each.
        /STACK:8388608)
    endif()
  endif()
endfunction()

add_executable(boris src/main.c)
target_link_libraries(boris PRIVATE boris_http)
boris_executable(boris)

enable_testing()
foreach(test_source ${BORIS_TESTS})
  get_filename_component(test_name ${test_source} NAME_WE)
  if(test_name STREQUAL "test_e2e")
    add_executable(${test_name} ${test_source} tests/mock_http.c)
    target_link_libraries(${test_name} PRIVATE boris_core)
  else()
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE boris_http)
  endif()
  target_include_directories(${test_name} PRIVATE tests)
  boris_executable(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
