find_package(Threads REQUIRED)

# The controller is Qt-free: it is the send policy between the widgets and the
# public core API, so it can be exercised without a display server.
add_executable(
  conversation-controller-test
  conversation_controller_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/ConversationController.cpp
)

target_include_directories(
  conversation-controller-test
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(
  conversation-controller-test
  PRIVATE
    local-notebook-core
    Threads::Threads
)

add_test(NAME conversation-controller COMMAND conversation-controller-test)
