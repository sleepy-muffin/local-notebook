cmake_minimum_required(VERSION 3.31)
project(local-notebook)

enable_testing()

add_subdirectory(external/shark)
add_subdirectory(core)
add_subdirectory(ui)
