cmake_minimum_required(VERSION 3.21)
project(repaper_tablet_mouse_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Qml Test)
add_executable(tablet-mouse-adapter-tests TabletMouseAdapterTest.cpp ../TabletMouseAdapter.h)
target_include_directories(tablet-mouse-adapter-tests PRIVATE ..)
target_link_libraries(tablet-mouse-adapter-tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Test)
enable_testing()
add_test(NAME tablet-mouse-adapter COMMAND tablet-mouse-adapter-tests)
set_tests_properties(tablet-mouse-adapter PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;REPAPER_APPLOAD_TABLET_INPUT=0")
