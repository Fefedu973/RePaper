cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(recalc VERSION 0.1.0 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  include(CTest)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent Sql Network)
find_package(Boost REQUIRED CONFIG)
add_library(recalc_core STATIC
  src/expression/Ast.cpp src/expression/Ast.hpp
  src/expression/Editor.cpp src/expression/Editor.hpp
  src/evaluation/Evaluator.cpp src/evaluation/Evaluator.hpp)
target_include_directories(recalc_core PUBLIC src ${Boost_INCLUDE_DIRS})
target_compile_features(recalc_core PUBLIC cxx_std_17)
target_link_libraries(recalc_core PUBLIC Qt6::Core)
add_executable(recalc
  src/app/main.cpp src/app/Calculator.cpp src/app/Calculator.hpp
  src/layout/MathCanvas.cpp src/layout/MathCanvas.hpp
  src/history/HistoryStore.cpp src/history/HistoryStore.hpp resources.qrc)
target_link_libraries(recalc PRIVATE recalc_core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent Qt6::Sql)
target_include_directories(recalc PRIVATE ../../shared/input)
target_sources(recalc PRIVATE ../../shared/ui/AppLoadViewport.cpp ../../shared/ui/AppLoadViewport.h)
target_link_libraries(recalc PRIVATE Qt6::Network)
install(TARGETS recalc RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(recalc_tests tests/CalculatorTests.cpp src/history/HistoryStore.cpp)
  target_link_libraries(recalc_tests PRIVATE recalc_core Qt6::Test Qt6::Sql)
  add_test(NAME recalc_semantics COMMAND recalc_tests)
  add_executable(recalc_ui_tests tests/CanvasTests.cpp resources.qrc
    src/app/Calculator.cpp src/layout/MathCanvas.cpp src/history/HistoryStore.cpp)
  target_link_libraries(recalc_ui_tests PRIVATE recalc_core Qt6::Test Qt6::Gui Qt6::Quick Qt6::Concurrent Qt6::Sql)
  add_test(NAME recalc_ui_semantics COMMAND recalc_ui_tests)
  set_tests_properties(recalc_ui_semantics PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
  if(NOT CMAKE_CROSSCOMPILING AND RECALC_APPLOAD_SOURCE)
    set(RECALC_TEST_APPLOAD_SOURCE "${RECALC_APPLOAD_SOURCE}")
    if(Qt6Core_VERSION VERSION_LESS 6.7)
      set(RECALC_TEST_APPLOAD_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/viewport-qt62-compat")
      file(COPY "${RECALC_APPLOAD_SOURCE}/src/qtfb" DESTINATION "${RECALC_TEST_APPLOAD_SOURCE}/src")
      file(READ "${RECALC_TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h" viewport_header)
      string(REPLACE "setFocusPolicy(Qt::StrongFocus);" "" viewport_header "${viewport_header}")
      file(WRITE "${RECALC_TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h" "${viewport_header}")
    endif()
    set(viewport_host "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/appload/device/native-viewport-host")
    add_executable(recalc_native_viewport_tests tests/NativeViewportUiTests.cpp resources.qrc
      src/app/Calculator.cpp src/layout/MathCanvas.cpp src/history/HistoryStore.cpp
      ../../shared/ui/AppLoadViewport.cpp ../../shared/ui/AppLoadViewport.h
      "${viewport_host}/NativeViewportHost.cpp" "${viewport_host}/NativeViewportHost.h"
      "${RECALC_TEST_APPLOAD_SOURCE}/src/qtfb/FBController.cpp"
      "${RECALC_TEST_APPLOAD_SOURCE}/src/qtfb/FBController.h")
    target_include_directories(recalc_native_viewport_tests PRIVATE "${viewport_host}" "${RECALC_TEST_APPLOAD_SOURCE}/src" ../../shared/ui)
    target_link_libraries(recalc_native_viewport_tests PRIVATE recalc_core Qt6::Test Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent Qt6::Sql Qt6::Network)
    add_test(NAME recalc_native_viewport COMMAND recalc_native_viewport_tests)
    set_tests_properties(recalc_native_viewport PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
  endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../shared/ui/PaperUi.cmake)
repaper_use_ui(recalc)
repaper_use_ui(recalc_ui_tests)
repaper_use_ui(recalc_native_viewport_tests)
