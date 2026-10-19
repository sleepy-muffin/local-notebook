cmake_minimum_required(VERSION 3.31)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# The application uses Qt Widgets without moc (no Q_OBJECT anywhere), so AUTOMOC
# stays off and the build is a plain C++23 compile.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC ON)

set(LOCAL_NOTEBOOK_UI_SOURCES
        src/settings/settings.cpp
        src/support/i18n.cpp

        src/ui/AssistantForm.cpp
        src/ui/AssistantPicker.cpp
        src/ui/BackgroundWidget.cpp
        src/ui/ConversationController.cpp
        src/ui/ConversationView.cpp
        src/ui/MainWindow.cpp
        src/ui/SessionEditor.cpp
        src/ui/SessionItemDelegate.cpp
        src/ui/SessionListModel.cpp
        src/ui/SettingsWindow.cpp
        src/ui/StatusNotice.cpp
        src/ui/SummaryReviewDialog.cpp
        src/ui/TasksWindow.cpp
        src/ui/theme.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/local-notebook.qrc
)

add_library(local-notebook-ui-core STATIC
        ${LOCAL_NOTEBOOK_UI_SOURCES}
)

target_include_directories(local-notebook-ui-core
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(local-notebook-ui-core
        PUBLIC
        local-notebook-core
        shark
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

add_executable(local-notebook
        src/main.cpp
)

target_link_libraries(local-notebook
        PRIVATE
        local-notebook-ui-core
)

# ========== ---------- ---------- ---------- ==========
#                          TOOLS
# ========== ---------- ---------- ---------- ==========

# Development-only executable used to regenerate README screenshots.
add_executable(local-notebook-screenshots
        tools/screenshots.cpp
)

target_link_libraries(local-notebook-screenshots
        PRIVATE
        local-notebook-ui-core
)

# ========== ---------- ---------- ---------- ==========
#                         CTESTS
# ========== ---------- ---------- ---------- ==========

# Regression tests for the UI-side send policy.
# The controller is Qt-free: it is the send policy between the widgets and the
# public core API, so it can be exercised without a display server.
add_executable(
        conversation-controller-test
        tests/conversation_controller_test.cpp
)

target_link_libraries(
        conversation-controller-test
        PRIVATE
        local-notebook-ui-core
)

add_test(NAME conversation-controller COMMAND conversation-controller-test)

