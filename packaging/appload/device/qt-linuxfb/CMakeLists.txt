cmake_minimum_required(VERSION 3.21)
project(repaper_qt_plugins LANGUAGES CXX)

# Rebuild only Qt's missing LinuxFB platform plugin using the exact matching
# reMarkable SDK; its installed private support libraries contain the rest.
find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS Core Gui Sql SqlPrivate FbSupportPrivate InputSupportPrivate DeviceDiscoverySupportPrivate)
find_package(SQLite3 REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(qlinuxfb MODULE
    upstream/main.cpp
    upstream/qlinuxfbintegration.cpp upstream/qlinuxfbintegration.h
    upstream/qlinuxfbscreen.cpp upstream/qlinuxfbscreen.h
)
target_compile_definitions(qlinuxfb PRIVATE QT_NO_FOREACH QT_PLUGIN)
target_link_libraries(qlinuxfb PRIVATE
    Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::GuiPrivate
    Qt6::FbSupportPrivate Qt6::InputSupportPrivate
)
target_link_options(qlinuxfb PRIVATE -Wl,--no-undefined)
set_target_properties(qlinuxfb PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/platforms"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../../lib"
)
install(TARGETS qlinuxfb LIBRARY DESTINATION plugins/platforms)

# The SDK also omits the QSQLITE driver required by the applications. Use its
# system SQLite library, as supported by upstream Qt's system_sqlite option.
add_library(qsqlite MODULE
    upstream-sqlite/smain.cpp
    upstream-sqlite/qsql_sqlite.cpp upstream-sqlite/qsql_sqlite_p.h
    upstream-sqlite/qsql_sqlite_vfs.cpp upstream-sqlite/qsql_sqlite_vfs_p.h
)
target_compile_definitions(qsqlite PRIVATE
    QT_PLUGIN QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_CONTEXTLESS_CONNECT
)
target_link_libraries(qsqlite PRIVATE
    Qt6::Core Qt6::CorePrivate Qt6::Sql Qt6::SqlPrivate SQLite::SQLite3
)
target_link_options(qsqlite PRIVATE -Wl,--no-undefined)
set_target_properties(qsqlite PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/sqldrivers"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../../lib"
)
install(TARGETS qsqlite LIBRARY DESTINATION plugins/sqldrivers)

# Qt 6.10.3's evdev tablet handler feeds logical screen pixels to a native-pixel
# QWindowSystemInterface API. Rebuild that handler with the reviewed coordinate
# correction, preserving the vendored upstream bytes and SDK plugin unchanged.
find_program(PATCH_EXECUTABLE patch REQUIRED)
set(tablet_source "${CMAKE_BINARY_DIR}/evdevtablet-patched")
file(COPY upstream-evdevtablet/ DESTINATION "${tablet_source}")
execute_process(
    COMMAND "${PATCH_EXECUTABLE}" --batch --forward --fuzz=0 -p1 -i
            "${CMAKE_CURRENT_SOURCE_DIR}/evdevtablet-native-coordinates.patch"
    WORKING_DIRECTORY "${tablet_source}"
    RESULT_VARIABLE tablet_patch_result
)
if(NOT tablet_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not apply the Qt evdev tablet coordinate patch")
endif()
add_library(qevdevtablet MODULE
    "${tablet_source}/plugin/main.cpp"
    "${tablet_source}/handler/qevdevtablethandler.cpp"
    "${tablet_source}/handler/qevdevtablethandler_p.h"
    "${tablet_source}/handler/qevdevtabletmanager.cpp"
    "${tablet_source}/handler/qevdevtabletmanager_p.h"
)
target_compile_definitions(qevdevtablet PRIVATE QT_NO_FOREACH QT_PLUGIN)
target_link_libraries(qevdevtablet PRIVATE
    Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::GuiPrivate
    Qt6::InputSupportPrivate Qt6::DeviceDiscoverySupportPrivate
)
target_link_options(qevdevtablet PRIVATE -Wl,--no-undefined)
set_target_properties(qevdevtablet PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/generic"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../../lib"
)
install(TARGETS qevdevtablet LIBRARY DESTINATION plugins/generic)

# Keep build directory paths out of plugin diagnostics/debug metadata so the
# same pinned source and SDK produce identical plugins in a fresh build tree.
foreach(plugin IN ITEMS qlinuxfb qsqlite qevdevtablet)
    target_compile_options(${plugin} PRIVATE
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/repaper-qt-plugin-source"
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/repaper-qt-plugin-build"
    )
    # SDK support archives contain LTO objects, whose debug metadata is also
    # generated at link time.
    target_link_options(${plugin} PRIVATE
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/repaper-qt-plugin-source"
        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/repaper-qt-plugin-build"
    )
endforeach()

add_executable(validate-plugins validate-plugins.cpp)
target_link_libraries(validate-plugins PRIVATE Qt6::Core Qt6::Sql)
