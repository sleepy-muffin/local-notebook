cmake_minimum_required(VERSION 3.31)
project(local-notebook)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SQLite3 REQUIRED)

# The reconstructed application core (design-1.md). It is Qt-free and headless
# so it can be unit-tested without a GUI; the context and UI layers join it in
# later milestones. The legacy application is parked in src/.old/ and is no
# longer built.
add_library(${PROJECT_NAME} STATIC
        src/model.hpp
        src/store/sqlite.hpp
        src/store/sqlite.cpp
        src/store/util.hpp
        src/store/util.cpp
        src/store/database.hpp
        src/store/database.cpp
        src/store/managers.hpp
        src/store/managers.cpp
)

target_include_directories(
        ${PROJECT_NAME}
        PUBLIC
        src
        ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(
        ${PROJECT_NAME}
        PUBLIC
        ${SQLite3_LIBRARIES}
)

enable_testing()

add_executable(local-notebook-tests
        tests/store_tests.cpp
)

target_link_libraries(
        local-notebook-tests
        PRIVATE
        ${PROJECT_NAME}
)

add_test(NAME store COMMAND local-notebook-tests)
