cmake_minimum_required(VERSION 3.16)
project(corelet
    VERSION 0.1.0
    DESCRIPTION "MeshCore companion app for the ClockworkPi uConsole"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Widgets, not Quick: the uConsole is a CM4 with no useful GPU for scene graph
# compositing, and a QML runtime costs both startup time and resident memory
# that a chat window does not need.
# Svg is for the sidebar's channel-type icons: they ship as stroke drawings on a
# 24x24 grid and are rasterised at whatever size and colour a row needs, which
# beats carrying a PNG per size, per colour, per screen density.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network Bluetooth Sql Svg)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(corelet
    src/main.cpp
    src/ui/icons/icons.qrc
    src/protocol/frame_codec.cpp
    src/protocol/transport.cpp
    src/protocol/tcp_transport.cpp
    src/protocol/ble_transport.cpp
    src/protocol/client.cpp
    src/model/history.cpp
    src/model/chat_model.cpp
    src/model/channel_model.cpp
    src/ui/theme.cpp
    src/ui/icons.cpp
    src/ui/channel_delegate.cpp
    src/ui/message_delegate.cpp
    src/ui/connect_dialog.cpp
    src/ui/add_channel_dialog.cpp
    src/ui/share_channel_dialog.cpp
    src/ui/node_pane.cpp
    src/ui/main_window.cpp
)

target_include_directories(corelet PRIVATE src)
target_link_libraries(corelet PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Bluetooth Qt6::Sql Qt6::Svg)

target_compile_definitions(corelet PRIVATE
    CORELET_VERSION="${PROJECT_VERSION}"
    # Qt's foreach/signals macros collide with anything; keep the namespace clean
    # so this code reads as plain C++.
    QT_NO_KEYWORDS)

if(NOT MSVC)
    target_compile_options(corelet PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# macOS denies Bluetooth to a process with no usage description, and only a
# bundle has somewhere to declare one. Run it as
# ./build/corelet.app/Contents/MacOS/corelet.
if(APPLE)
    # Qt asks macOS for Bluetooth access through a permission plugin, and that
    # plugin is static: without it linked in, checkPermission() answers Denied
    # and the app refuses to scan on a Mac that would have allowed it. Qt6Core
    # defines the target when it is available, hence the guard rather than a
    # find_package of its own.
    if(TARGET Qt6::QDarwinBluetoothPermissionPlugin)
        target_link_libraries(corelet PRIVATE Qt6::QDarwinBluetoothPermissionPlugin)
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER org.umeshcore.corelet)
    set(MACOSX_BUNDLE_BUNDLE_NAME corelet)
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    set_source_files_properties(assets/corelet.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(corelet PRIVATE assets/corelet.icns)
    set_target_properties(corelet PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE corelet.icns
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/etc/Info.plist.in)
endif()

include(GNUInstallDirs)
install(TARGETS corelet
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
install(FILES etc/corelet.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES assets/corelet.png
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)

message(STATUS "corelet ${PROJECT_VERSION} (Qt ${Qt6_VERSION})")

include(CTest)
if(BUILD_TESTING)
    add_executable(history-test
        tests/history_test.cpp
        src/model/history.cpp)
    target_include_directories(history-test PRIVATE src)
    target_link_libraries(history-test PRIVATE Qt6::Core Qt6::Sql)
    target_compile_definitions(history-test PRIVATE QT_NO_KEYWORDS)
    if(NOT MSVC)
        target_compile_options(history-test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME history-test COMMAND history-test)

    add_executable(chat-model-test
        tests/chat_model_test.cpp
        src/model/chat_model.cpp)
    target_include_directories(chat-model-test PRIVATE src)
    target_link_libraries(chat-model-test PRIVATE Qt6::Core)
    target_compile_definitions(chat-model-test PRIVATE QT_NO_KEYWORDS)
    if(NOT MSVC)
        target_compile_options(chat-model-test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME chat-model-test COMMAND chat-model-test)
endif()
