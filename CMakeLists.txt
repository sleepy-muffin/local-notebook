cmake_minimum_required(VERSION 3.31)
project(local-notebook)

enable_testing()

# Qt 6.8 requires Windows 10, so declare that as the minimum target for every
# translation unit. This also stops Boost.Asio from assuming Windows 7, and
# NOMINMAX/WIN32_LEAN_AND_MEAN keep the Windows SDK from defining `min`, `max`
# and COM's `interface` macros, which otherwise collide with ordinary C++
# identifiers.
if(WIN32)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
endif()

# Boost.Beast/Asio translation units are large enough to exceed MSVC's default
# object-file section limit.
if(MSVC)
    add_compile_options(/bigobj)
endif()

# No translation unit uses C++20 modules. CMake otherwise scans every source
# with clang-scan-deps whenever the compiler supports it (clang does), and that
# tool is packaged separately as clang-tools rather than with the compiler, so
# builds with only clang installed fail. It also adds a scan step per file for
# no benefit. Override the cache entry to enable scanning.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL
    "Scan C++ sources for module dependencies")

# Only the standard-library core, the Boost.JSON helpers and the SQLite wrapper
# are used. Turning the image and audio modules off also stops CMake from
# probing for OpenGL, JPEG, PNG and the audio codecs. The options are forced so
# a cache left over from an earlier configure cannot resurrect those probes.
set(SHARK_BUILD_IMAGE OFF CACHE BOOL "Build shark's image module" FORCE)
set(SHARK_BUILD_AUDIO OFF CACHE BOOL "Build shark's audio module" FORCE)

add_subdirectory(external/shark)
add_subdirectory(core)
add_subdirectory(ui)
