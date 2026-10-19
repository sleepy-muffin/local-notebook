cmake_minimum_required(VERSION 3.31)
project(local-notebook-core)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

        src/store/database.hpp
        src/store/database.cpp
        src/store/managers.hpp
        src/store/managers.cpp

        src/context/context.hpp
        src/context/context.cpp

        src/net/protocol.hpp
        src/net/protocol.cpp
        src/net/proxy.hpp
        src/net/proxy.cpp
        src/net/http.hpp
        src/net/http.cpp
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

target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE
        shark
        Boost::json
        Boost::regex
        Boost::url
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Regression tests for the public core API.
add_subdirectory(tests)
