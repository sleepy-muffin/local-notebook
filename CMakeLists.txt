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

add_subdirectory(external/shark)
add_subdirectory(core)
add_subdirectory(ui)
