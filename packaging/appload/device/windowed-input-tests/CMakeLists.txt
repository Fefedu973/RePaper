cmake_minimum_required(VERSION 3.16)
project(repaper_windowed_input_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Test)
if(NOT FBCONTROLLER_SOURCE)
  message(FATAL_ERROR "Set FBCONTROLLER_SOURCE to the private source copy")
endif()
add_executable(fbcontroller-input-tests
  FBControllerInputTests.cpp
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.cpp"
  "${FBCONTROLLER_SOURCE}/src/qtfb/FBController.h")
target_include_directories(fbcontroller-input-tests PRIVATE "${FBCONTROLLER_SOURCE}/src/qtfb")
target_link_libraries(fbcontroller-input-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
