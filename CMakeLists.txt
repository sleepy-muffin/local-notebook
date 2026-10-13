cmake_minimum_required(VERSION 3.31)
project(local-notebook)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenGL REQUIRED)
find_package(SQLite3 REQUIRED)

find_package(Qt6 REQUIRED COMPONENTS
        Core
        Gui
        Widgets
        Network
)

add_executable(${PROJECT_NAME}
        src/db.hpp
        src/db.cpp
        src/ui.hpp
        src/external.hpp
        src/external.cpp
        src/context_manager.cpp
        src/proxy.hpp
        src/proxy.cpp
        src/launcher.cpp
        src/ui/Logon.cpp
        src/ui/Logon.hpp
        src/ui/MainWindow.cpp
        src/ui/MainWindow.hpp
        src/ui/theme.cpp
        src/ui/theme.hpp
        src/ui/metrics.hpp
        src/ui/BackgroundWidget.cpp
        src/ui/BackgroundWidget.hpp
        src/ui/SessionEditor.cpp
        src/ui/SessionEditor.hpp
        src/ui/SessionListModel.cpp
        src/ui/SessionListModel.hpp
        src/ui/SessionItemDelegate.hpp
        src/ui/SessionItemDelegate.cpp
        src/ui/SettingsWindow.cpp
        src/ui/SettingsWindow.hpp
        src/external_tools.cpp
        src/external_tools.hpp
        src/generation_task.hpp
        src/ui/ConversationView.cpp
        src/ui/ConversationView.hpp
        src/ui/ConversationController.cpp
        src/ui/ConversationController.hpp
)

qt_add_resources(${PROJECT_NAME} "theme_resources"
        PREFIX "/"
        FILES
        themes/theme.json
)

add_subdirectory(
        external/mmc build/mmc
)

target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
        src/
        ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE
        mmc
        ${SQLite3_LIBRARIES}

        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
)
