add_executable(
  assistant-summary-log-test
  assistant_summary_log_test.cpp
)

target_link_libraries(
  assistant-summary-log-test
  PRIVATE
    local-notebook-core
)

add_test(NAME assistant-summary-log COMMAND assistant-summary-log-test)

add_executable(
  url-validation-test
  url_validation_test.cpp
)

target_link_libraries(
  url-validation-test
  PRIVATE
    local-notebook-core
)

add_test(NAME url-validation COMMAND url-validation-test)

add_executable(
  http-generation-test
  http_generation_test.cpp
)

target_link_libraries(
  http-generation-test
  PRIVATE
    local-notebook-core
    Threads::Threads
)

add_test(NAME http-generation COMMAND http-generation-test)

add_executable(
  length-truncation-test
  length_truncation_test.cpp
)

target_link_libraries(
  length-truncation-test
  PRIVATE
    local-notebook-core
    Threads::Threads
)

add_test(NAME length-truncation COMMAND length-truncation-test)

add_executable(
  search-parse-test
  search_parse_test.cpp
)

target_include_directories(
  search-parse-test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(
  search-parse-test
  PRIVATE
    local-notebook-core
    Boost::regex
)

add_test(NAME search-parse COMMAND search-parse-test)

add_executable(
  tools-paths-test
  tools_paths_test.cpp
)

target_include_directories(
  tools-paths-test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(
  tools-paths-test
  PRIVATE
    local-notebook-core
)

add_test(NAME tools-paths COMMAND tools-paths-test)

add_executable(
  session-directories-test
  session_directories_test.cpp
)

target_include_directories(
  session-directories-test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(
  session-directories-test
  PRIVATE
    local-notebook-core
    shark::sqlite3
)

add_test(NAME session-directories COMMAND session-directories-test)

add_executable(
  assistant-settings-test
  assistant_settings_test.cpp
)

target_include_directories(
  assistant-settings-test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(
  assistant-settings-test
  PRIVATE
    local-notebook-core
)

add_test(NAME assistant-settings COMMAND assistant-settings-test)

add_executable(
  instance-lock-test
  instance_lock_test.cpp
)

target_link_libraries(
  instance-lock-test
  PRIVATE
    local-notebook-core
)

add_test(NAME instance-lock COMMAND instance-lock-test)

add_executable(
  assistant-memory-filter-test
  assistant_memory_filter_test.cpp
)

target_link_libraries(
  assistant-memory-filter-test
  PRIVATE
    local-notebook-core
)

add_test(NAME assistant-memory-filter COMMAND assistant-memory-filter-test)

add_executable(
  concurrent-sessions-test
  concurrent_sessions_test.cpp
)

target_link_libraries(
  concurrent-sessions-test
  PRIVATE
    local-notebook-core
    Threads::Threads
)

add_test(NAME concurrent-sessions COMMAND concurrent-sessions-test)
