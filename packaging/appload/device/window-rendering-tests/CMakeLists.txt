cmake_minimum_required(VERSION 3.21)
project(repaper_window_rendering_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Test)
if(NOT FBCONTROLLER_SOURCE)
  message(FATAL_ERROR "Set FBCONTROLLER_SOURCE to the private patched source copy")
endif()
add_executable(window-rendering-tests WindowRenderingTests.cpp
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.cpp"
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.h")
target_include_directories(window-rendering-tests PRIVATE "${FBCONTROLLER_SOURCE}/src/qtfb")
target_link_libraries(window-rendering-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_executable(window-input-regressions ../windowed-input-tests/FBControllerInputTests.cpp
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.cpp"
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.h")
target_include_directories(window-input-regressions PRIVATE "${FBCONTROLLER_SOURCE}/src/qtfb")
target_link_libraries(window-input-regressions PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
