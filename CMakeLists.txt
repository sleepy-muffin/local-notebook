cmake_minimum_required(VERSION 3.31)
project(local-notebook)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The application uses Qt Widgets without moc (no Q_OBJECT anywhere), so
# AUTOMOC stays off and the build is a plain C++23 compile.
set(CMAKE_AUTOMOC OFF)

find_package(SQLite3 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# The reconstructed application core (design-1.md). The legacy application is
# parked in src/.old/ and is no longer built.
add_library(local-notebook-core STATIC
        src/model.hpp

        src/support/log.hpp
        src/support/generation_task.hpp

        src/store/sqlite.hpp
        src/store/sqlite.cpp
        src/store/util.hpp
        src/store/util.cpp
        src/store/database.hpp
        src/store/database.cpp
        src/store/managers.hpp
        src/store/managers.cpp

        src/app/metrics.hpp
        src/app/settings.hpp
        src/app/settings.cpp
        src/app/assistant_io.hpp
        src/app/assistant_io.cpp
        src/app/application.hpp
        src/app/application.cpp

        src/context/context.hpp
        src/context/context.cpp

        src/net/proxy.hpp
        src/net/proxy.cpp
        src/net/tools.hpp
        src/net/tools.cpp
        src/net/client.hpp
        src/net/client.cpp

        src/ui/theme.hpp
        src/ui/theme.cpp
        src/ui/BackgroundWidget.hpp
        src/ui/BackgroundWidget.cpp
        src/ui/dialog_style.hpp
        src/ui/SpinBox.hpp
        src/ui/SessionListModel.hpp
        src/ui/SessionListModel.cpp
        src/ui/SessionItemDelegate.hpp
        src/ui/SessionItemDelegate.cpp
        src/ui/ConversationController.hpp
        src/ui/ConversationController.cpp
        src/ui/ConversationView.hpp
        src/ui/ConversationView.cpp
        src/ui/AssistantForm.hpp
        src/ui/AssistantForm.cpp
        src/ui/AssistantPicker.hpp
        src/ui/AssistantPicker.cpp
        src/ui/SettingsWindow.hpp
        src/ui/SettingsWindow.cpp
        src/ui/SessionEditor.hpp
        src/ui/SessionEditor.cpp
        src/ui/MainWindow.hpp
        src/ui/MainWindow.cpp
)

target_include_directories(
        local-notebook-core
        PUBLIC
        src
        ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(
        local-notebook-core
        PUBLIC
        ${SQLite3_LIBRARIES}
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
)

enable_testing()

# The application itself. The core library is named local-notebook-core so this
# executable can take the project's name.
add_executable(local-notebook
        src/main.cpp
)

target_link_libraries(
        local-notebook
        PRIVATE
        local-notebook-core
)

add_executable(local-notebook-tests
        tests/store_tests.cpp
)

target_link_libraries(
        local-notebook-tests
        PRIVATE
        local-notebook-core
)

add_test(NAME store COMMAND local-notebook-tests)

# Offscreen UI tests (window chrome, settings persistence, the shared assistant
# form). They run against the offscreen platform plugin so no display is needed.
add_executable(local-notebook-ui-tests
        tests/ui_tests.cpp
)

target_link_libraries(
        local-notebook-ui-tests
        PRIVATE
        local-notebook-core
)

add_test(NAME ui COMMAND local-notebook-ui-tests)
set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
