cmake_minimum_required(VERSION 3.31)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# The application uses Qt Widgets without moc (no Q_OBJECT anywhere), so AUTOMOC
# stays off and the build is a plain C++23 compile.
set(CMAKE_AUTOMOC OFF)

# The composer icons, the default theme and the application icon are compiled
# into the binary, so a deployed build carries them without loose files beside
# the executable.
set(CMAKE_AUTORCC ON)

add_executable(local-notebook
  src/main.cpp

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

# The UI compiles against its own sources and the public core headers only.
# core/src is never on this include path.
target_include_directories(local-notebook PRIVATE src)

target_link_libraries(local-notebook
  PRIVATE
    local-notebook-core
    mmc
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# Regression tests for the UI-side send policy. They link only the public core
# headers, never core/src.
add_subdirectory(tests)
