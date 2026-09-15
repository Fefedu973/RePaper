cmake_minimum_required(VERSION 3.21)
project(repaper_viewport_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Network Test)
if(NOT APPLOAD_SOURCE)
  message(FATAL_ERROR "Set APPLOAD_SOURCE to the patched AppLoad stage")
endif()
get_filename_component(REPAPER_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(TEST_APPLOAD_SOURCE "${APPLOAD_SOURCE}")
if(Qt6Core_VERSION VERSION_LESS 6.7)
  # QQuickItem's focusPolicy exists in the target SDK but not PC Qt 6.2.
  # Keep the production drawing/input source exact; remove only that unused
  # constructor call in an isolated test header.
  set(TEST_APPLOAD_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/qt62-compat")
  file(COPY "${APPLOAD_SOURCE}/src/qtfb" DESTINATION "${TEST_APPLOAD_SOURCE}/src")
  file(READ "${TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h" FBC_HEADER)
  string(REPLACE "setFocusPolicy(Qt::StrongFocus);" "" FBC_HEADER "${FBC_HEADER}")
  file(WRITE "${TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h" "${FBC_HEADER}")
endif()
add_executable(viewport-tests ViewportTests.cpp
  ../NativeViewportHost.cpp ../NativeViewportHost.h
  "${REPAPER_REPO}/shared/ui/AppLoadViewport.cpp"
  "${REPAPER_REPO}/shared/ui/AppLoadViewport.h"
  "${TEST_APPLOAD_SOURCE}/src/qtfb/FBController.cpp"
  "${TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h")
target_include_directories(viewport-tests PRIVATE .. "${REPAPER_REPO}/apps/repdf/src" "${TEST_APPLOAD_SOURCE}/src")
target_link_libraries(viewport-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Test)
enable_testing()
add_test(NAME responsive_viewport COMMAND viewport-tests)
set_tests_properties(responsive_viewport PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
