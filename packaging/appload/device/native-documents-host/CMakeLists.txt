cmake_minimum_required(VERSION 3.21)
project(repaper_native_documents_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Qml Network Test)
enable_testing()

add_executable(agenda-note-context-tests tests/AgendaNoteContextTest.cpp)
target_link_libraries(agenda-note-context-tests PRIVATE Qt6::Core Qt6::Test)
target_include_directories(agenda-note-context-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME agenda-note-context-tests COMMAND agenda-note-context-tests)

add_executable(native-document-host-tests
    NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeDocumentHostTest.cpp)
target_link_libraries(native-document-host-tests PRIVATE Qt6::Core Qt6::Qml Qt6::Network Qt6::Test)
target_include_directories(native-document-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(native-document-host-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME native-document-host-tests COMMAND native-document-host-tests)
set_tests_properties(native-document-host-tests PROPERTIES TIMEOUT 60)

add_executable(native-import-host-tests
    NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeImportHostTest.cpp)
target_link_libraries(native-import-host-tests PRIVATE Qt6::Core Qt6::Qml Qt6::Network Qt6::Test)
target_include_directories(native-import-host-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(native-import-host-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME native-import-host-tests COMMAND native-import-host-tests)
set_tests_properties(native-import-host-tests PROPERTIES TIMEOUT 60)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeDocumentsAdapterTest.cpp")
    find_package(Qt6 6.2 REQUIRED COMPONENTS Gui Quick QuickControls2)
    add_executable(native-document-adapter-tests
        NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeDocumentsAdapterTest.cpp)
    target_link_libraries(native-document-adapter-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Test)
    target_include_directories(native-document-adapter-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_source_files_properties(NativeDocumentsAdapter.qml PROPERTIES QT_RESOURCE_ALIAS RePaperNativeDocuments.qml)
    set_source_files_properties(NativeImportsAdapter.qml PROPERTIES QT_RESOURCE_ALIAS RePaperNativeImports.qml)
    qt_add_resources(native-document-adapter-tests native_documents_adapter
        PREFIX /appload/qml FILES NativeDocumentsAdapter.qml NativeImportsAdapter.qml)
    set_source_files_properties(tests/AdapterFixture.qml PROPERTIES QT_RESOURCE_ALIAS AdapterFixture.qml)
    qt_add_resources(native-document-adapter-tests native_documents_fixture
        PREFIX /native-documents-tests FILES tests/AdapterFixture.qml)
    add_test(NAME native-document-adapter-tests COMMAND native-document-adapter-tests)
    set_tests_properties(native-document-adapter-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    add_executable(native-document-types-tests
        NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeDocumentTypesTest.cpp)
    target_link_libraries(native-document-types-tests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
    target_include_directories(native-document-types-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    qt_add_resources(native-document-types-tests native_documents_typed_adapter
        PREFIX /appload/qml FILES NativeDocumentsAdapter.qml NativeImportsAdapter.qml)
    set_source_files_properties(tests/NativeTypesFixture.qml PROPERTIES QT_RESOURCE_ALIAS NativeTypesFixture.qml)
    qt_add_resources(native-document-types-tests native_documents_typed_fixture
        PREFIX /native-documents-tests FILES tests/NativeTypesFixture.qml)
    add_test(NAME native-document-types-tests COMMAND native-document-types-tests)
    set_tests_properties(native-document-types-tests PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Gui Quick)
add_executable(native-import-adapter-tests
    NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeImportsAdapterTest.cpp)
target_link_libraries(native-import-adapter-tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
target_include_directories(native-import-adapter-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(NativeDocumentsAdapter.qml PROPERTIES QT_RESOURCE_ALIAS RePaperNativeDocuments.qml)
set_source_files_properties(NativeImportsAdapter.qml PROPERTIES QT_RESOURCE_ALIAS RePaperNativeImports.qml)
qt_add_resources(native-import-adapter-tests native_import_adapter
    PREFIX /appload/qml FILES NativeDocumentsAdapter.qml NativeImportsAdapter.qml)
set_source_files_properties(tests/ImportTypesFixture.qml PROPERTIES QT_RESOURCE_ALIAS ImportTypesFixture.qml)
qt_add_resources(native-import-adapter-tests native_import_fixture
    PREFIX /native-documents-tests FILES tests/ImportTypesFixture.qml)
add_test(NAME native-import-adapter-tests COMMAND native-import-adapter-tests)
set_tests_properties(native-import-adapter-tests PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

add_executable(native-agenda-folder-tests
    NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeAgendaFolderTest.cpp)
target_link_libraries(native-agenda-folder-tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
target_include_directories(native-agenda-folder-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
qt_add_resources(native-agenda-folder-tests native_agenda_folder_adapter
    PREFIX /appload/qml FILES NativeDocumentsAdapter.qml NativeImportsAdapter.qml)
set_source_files_properties(tests/FolderTypesFixture.qml PROPERTIES QT_RESOURCE_ALIAS FolderTypesFixture.qml)
qt_add_resources(native-agenda-folder-tests native_agenda_folder_fixture
    PREFIX /native-documents-tests FILES tests/FolderTypesFixture.qml)
add_test(NAME native-agenda-folder-tests COMMAND native-agenda-folder-tests)
set_tests_properties(native-agenda-folder-tests PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

add_executable(native-agenda-initialization-tests
    NativeDocumentHost.cpp NativeDocumentHost.h tests/NativeAgendaInitializationTest.cpp)
target_link_libraries(native-agenda-initialization-tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
target_include_directories(native-agenda-initialization-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
qt_add_resources(native-agenda-initialization-tests native_agenda_initialization_adapter
    PREFIX /appload/qml FILES NativeDocumentsAdapter.qml NativeImportsAdapter.qml)
set_source_files_properties(tests/AgendaInitializationFixture.qml PROPERTIES QT_RESOURCE_ALIAS AgendaInitializationFixture.qml)
qt_add_resources(native-agenda-initialization-tests native_agenda_initialization_fixture
    PREFIX /native-documents-tests FILES tests/AgendaInitializationFixture.qml)
add_test(NAME native-agenda-initialization-tests COMMAND native-agenda-initialization-tests)
set_tests_properties(native-agenda-initialization-tests PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
