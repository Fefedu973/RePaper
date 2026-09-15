cmake_minimum_required(VERSION 3.16)
project(repaper_native_keyboard_host_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Network Test)
if(NOT APPLOAD_SOURCE)
    message(FATAL_ERROR "Set APPLOAD_SOURCE to the private AppLoad source copy")
endif()
add_executable(native-keyboard-host-tests
    NativeKeyboardHostTests.cpp
    ../NativeKeyboardHost.cpp ../NativeKeyboardHost.h
    "${APPLOAD_SOURCE}/src/qtfb/FBController.cpp"
    "${APPLOAD_SOURCE}/src/qtfb/FBController.h"
)
target_include_directories(native-keyboard-host-tests PRIVATE "${APPLOAD_SOURCE}/src" ..)
target_link_libraries(native-keyboard-host-tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Test
)
