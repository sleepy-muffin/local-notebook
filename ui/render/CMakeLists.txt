cmake_minimum_required(VERSION 3.31)

project(local-notebook-render
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The transcript renderer: document model, layout, hit testing and selection.
# It deliberately has no Qt dependency; the UI supplies a Qt-backed
# TextBackend/Canvas, and the tests supply a fixed-width fake. Keeping this
# library pure is what makes the geometry testable without a display server.
add_library(${PROJECT_NAME} STATIC
        include/render/types.hpp
        include/render/document.hpp
        include/render/backend.hpp
        include/render/layout.hpp
        src/layout.cpp
)

target_include_directories(${PROJECT_NAME}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ========== ---------- ---------- ---------- ==========
#                         CTESTS
# ========== ---------- ---------- ---------- ==========

add_executable(
        render-layout-test
        tests/layout_test.cpp
)

target_link_libraries(
        render-layout-test
        PRIVATE
        ${PROJECT_NAME}
)

add_test(NAME render-layout COMMAND render-layout-test)
