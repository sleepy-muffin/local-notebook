cmake_minimum_required(VERSION 3.31)

project(
        local-notebook-core
    LANGUAGES CXX
)


# ========== ---------- ---------- ---------- ==========
#                   PLATFORM VARIABLES
# ========== ---------- ---------- ---------- ==========


# Platform string
if(WIN32)
    set(PLATFORM "windows")
elseif(APPLE)
    set(PLATFORM "macos")
elseif(UNIX)
    set(PLATFORM "linux")
else()
    set(PLATFORM "unknown")
endif()


# Architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(ARCH "arm64")
else()
    set(ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ========== ---------- ---------- ---------- ==========
#                         SET UP
# ========== ---------- ---------- ---------- ==========


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SQLite3 REQUIRED)
find_package(Boost REQUIRED COMPONENTS json regex url)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# The application core. Only `include/` is public: the UI talks to the core
# through `local-notebook-core/interface.hpp` and never sees a store, network or
# executor type.
add_library(${PROJECT_NAME}
        STATIC
        include/local-notebook-core/api_protocol.hpp
        include/local-notebook-core/error_code.hpp
        include/local-notebook-core/generation_task.hpp
        include/local-notebook-core/interface.hpp
        include/local-notebook-core/model.hpp
        include/local-notebook-core/summary.hpp

        src/interface.cpp

        src/utils/util.hpp
        src/utils/util.cpp
        src/utils/lock.hpp
        src/utils/lock.cpp

        src/store/database.hpp
        src/store/database.cpp
        src/store/managers.hpp
        src/store/managers.cpp

        src/context/context.hpp
        src/context/context.cpp

        src/net/protocol.hpp
        src/net/protocol.cpp
        src/net/tools.hpp
        src/net/tools.cpp
        src/net/search_results.hpp
        src/net/search.hpp
        src/net/search.cpp
        src/net/client.hpp
        src/net/client.cpp

        src/app/assistant_io.hpp
        src/app/assistant_io.cpp
)


set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "${PROJECT_NAME}-${PLATFORM}-${ARCH}"
)

target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${SQLite3_INCLUDE_DIRS}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
        ${PROJECT_NAME}
        PUBLIC
        # The tests (and the UI's loopback-server test) include Boost.Asio
        # directly. Keeping the Boost targets public makes their include
        # directories available to those consumers; on Windows the headers live
        # in the vcpkg tree rather than the compiler's default include path.
        Boost::json
        PRIVATE
        shark::common
        shark::net
        shark::sqlite3
        ${SQLite3_LIBRARIES}
        Threads::Threads
)

# The TLS client loads the OS root store on Windows so HTTPS works without a
# bundled CA file.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE crypt32)
endif()

# ========== ---------- ---------- ---------- ==========
#                          TEST
# ========== ---------- ---------- ---------- ==========


# Regression tests for the public core API.
add_subdirectory(tests)
