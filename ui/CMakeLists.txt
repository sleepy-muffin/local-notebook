cmake_minimum_required(VERSION 3.31)

project(local-notebook-ui
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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# The application uses Qt Widgets without moc (no Q_OBJECT anywhere), so AUTOMOC
# stays off and the build is a plain C++23 compile.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC ON)

# The transcript renderer is UI-specific: its backend and canvas are Qt types,
# while the layout core stays Qt-free so its geometry tests can link it alone.
add_subdirectory(render)

set(LOCAL_NOTEBOOK_UI_SOURCES
        src/settings/settings.cpp
        src/support/i18n.cpp

        src/ui/AssistantForm.cpp
        src/ui/AssistantPicker.cpp
        src/ui/BackgroundWidget.cpp
        src/ui/ConversationController.cpp
        src/ui/ConversationView.cpp
        src/ui/MainWindow.cpp
        src/ui/MessageDocument.cpp
        src/ui/ScrollBar.cpp
        src/ui/SessionEditor.cpp
        src/ui/SessionItemDelegate.cpp
        src/ui/SessionListModel.cpp
        src/ui/SettingsWindow.cpp
        src/ui/StatusNotice.cpp
        src/ui/SummaryReviewDialog.cpp
        src/ui/TasksWindow.cpp
        src/ui/TranscriptCanvas.cpp
        src/ui/theme.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/local-notebook.qrc
)

add_library(${PROJECT_NAME}-core STATIC
        ${LOCAL_NOTEBOOK_UI_SOURCES}
)


set_target_properties(${PROJECT_NAME}-core PROPERTIES
        OUTPUT_NAME "${PROJECT_NAME}-core-${PLATFORM}-${ARCH}"
)

target_include_directories(${PROJECT_NAME}-core
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME}-core
        PUBLIC
        local-notebook-core
        local-notebook-render
        shark::common
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
)

# ========== ---------- ---------- ---------- ==========
#             WAYLAND TOPLEVEL ICONS (LINUX)
# ========== ---------- ---------- ---------- ==========

# Qt 6.8's Wayland platform plugin does not implement xdg-toplevel-icon-v1, so
# QApplication::setWindowIcon never reaches the compositor there (see
# src/wayland_icon.cpp). The application talks to the compositor directly when
# libwayland-client is available; builds without it simply keep the icon Qt
# provides. The protocol files were generated once with wayland-scanner 1.23.1
# from the staging xdg-toplevel-icon-v1.xml and are checked in, so no scanner
# is needed to build.
#
# Qt 6.5 is required as well: QNativeInterface::QWaylandApplication, the only
# public way to reach the compositor's wl_display without QtWayland's private
# headers, does not exist before it. With an older Qt (Ubuntu 24.04 ships
# 6.4.2) the block stays off instead of failing to compile, and the icon stays
# with Qt.
set(LOCAL_NOTEBOOK_WAYLAND_ICON_SOURCES)
set(LOCAL_NOTEBOOK_WAYLAND_ICON_LIBRARIES)
set(LOCAL_NOTEBOOK_WAYLAND_ICON_DEFINITIONS)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)

        if(WAYLAND_CLIENT_FOUND AND Qt6Gui_VERSION VERSION_GREATER_EQUAL 6.5)
            enable_language(C)
            set(LOCAL_NOTEBOOK_WAYLAND_ICON_SOURCES
                    src/wayland/xdg-toplevel-icon-v1-protocol.c
            )
            set(LOCAL_NOTEBOOK_WAYLAND_ICON_LIBRARIES PkgConfig::WAYLAND_CLIENT)
            set(LOCAL_NOTEBOOK_WAYLAND_ICON_DEFINITIONS LNB_HAVE_WAYLAND_ICON)

            # glibc before 2.34 keeps shm_open in librt; the shared memory
            # fallback in src/wayland_icon.cpp needs it there.
            find_library(LOCAL_NOTEBOOK_RT_LIBRARY rt)
            if(LOCAL_NOTEBOOK_RT_LIBRARY)
                list(APPEND LOCAL_NOTEBOOK_WAYLAND_ICON_LIBRARIES
                        ${LOCAL_NOTEBOOK_RT_LIBRARY})
            endif()
        endif()
    endif()
endif()

# ========== ---------- ---------- ---------- ==========
#                           UI
# ========== ---------- ---------- ---------- ==========

add_executable(${PROJECT_NAME}
        src/main.cpp
        src/wayland_icon.cpp
        ${LOCAL_NOTEBOOK_WAYLAND_ICON_SOURCES}
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        ${PROJECT_NAME}-core
        ${LOCAL_NOTEBOOK_WAYLAND_ICON_LIBRARIES}
)

target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        ${LOCAL_NOTEBOOK_WAYLAND_ICON_DEFINITIONS}
)


set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "local-notebook-${PLATFORM}-${ARCH}"
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

