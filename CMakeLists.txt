cmake_minimum_required(VERSION 3.16)
project(corelet
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

# Resolve once while configuring so generated inputs exist, then again before
# every build so a tag, commit or dirty tracked file is reflected without a
# manual reconfigure. Packaging passes CORELET_VERSION_MANIFEST to freeze all
# outputs to the same Git observation while it builds from a staged copy.
set(CORELET_VERSION_MANIFEST "" CACHE FILEPATH
    "Frozen version manifest supplied by a packaging build")
set(CORELET_GENERATED_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/corelet-version.cmake)
set(CORELET_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/version.h)
set(CORELET_MANPAGE ${CMAKE_CURRENT_BINARY_DIR}/corelet.1)
set(CORELET_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Corelet-Info.plist)
set(version_command
    ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUTPUT_MANIFEST=${CORELET_GENERATED_MANIFEST}
    -DOUTPUT_HEADER=${CORELET_VERSION_HEADER}
    -DOUTPUT_MANPAGE=${CORELET_MANPAGE}
    -DMANPAGE_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/etc/corelet.1.in
    -DOUTPUT_PLIST=${CORELET_INFO_PLIST}
    -DPLIST_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/etc/Info.plist.in)
if(CORELET_VERSION_MANIFEST)
    list(APPEND version_command -DINPUT_MANIFEST=${CORELET_VERSION_MANIFEST})
endif()
list(APPEND version_command -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake)
execute_process(COMMAND ${version_command} RESULT_VARIABLE version_status)
if(NOT version_status EQUAL 0)
    message(FATAL_ERROR "cannot resolve the Corelet version")
endif()
include(${CORELET_GENERATED_MANIFEST})

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

add_custom_target(corelet-version
    BYPRODUCTS ${CORELET_GENERATED_MANIFEST} ${CORELET_VERSION_HEADER}
               ${CORELET_MANPAGE} ${CORELET_INFO_PLIST}
    COMMAND ${version_command}
    COMMENT "Resolving the version from git"
    VERBATIM)
add_dependencies(corelet corelet-version)

target_include_directories(corelet PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(corelet PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Bluetooth Qt6::Sql Qt6::Svg)

target_compile_definitions(corelet PRIVATE
    # Qt's foreach/signals macros collide with anything; keep the namespace clean
    # so this code reads as plain C++.
    QT_NO_KEYWORDS)

if(NOT MSVC)
    target_compile_options(corelet PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# macOS denies Bluetooth to a process with no usage description, and only a
# bundle has somewhere to declare one. Run it as
# ./build/Corelet.app/Contents/MacOS/Corelet.
if(APPLE)
    # Qt asks macOS for Bluetooth access through a permission plugin, and that
    # plugin is static: without it linked in, checkPermission() answers Denied
    # and the app refuses to scan on a Mac that would have allowed it. Qt6Core
    # defines the target when it is available, hence the guard rather than a
    # find_package of its own.
    if(TARGET Qt6::QDarwinBluetoothPermissionPlugin)
        target_link_libraries(corelet PRIVATE Qt6::QDarwinBluetoothPermissionPlugin)
    endif()

    # Reverse-DNS of a domain the author controls. github.io is the usual stand-in
    # when there is no vanity domain, and it is what Flathub asks for too, so the
    # same identifier works on both stores if this is ever published.
    set_source_files_properties(assets/corelet.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(corelet PRIVATE assets/corelet.icns)
    # Finder and the menu bar show the bundle name, so the Mac build capitalises
    # its output. The Linux binary stays lowercase: it is typed and it is what the
    # desktop file's Exec= names.
    set_target_properties(corelet PROPERTIES
        OUTPUT_NAME Corelet
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE corelet.icns
        MACOSX_BUNDLE_INFO_PLIST ${CORELET_INFO_PLIST})
endif()

include(GNUInstallDirs)

install(TARGETS corelet
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
install(FILES etc/corelet.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES assets/corelet.png
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps)
install(FILES ${CORELET_MANPAGE}
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

message(STATUS "corelet ${CORELET_VERSION} (Qt ${Qt6_VERSION})")

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

    add_test(NAME version-test
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_test.cmake)
endif()
