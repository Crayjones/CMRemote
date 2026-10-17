cmake_minimum_required(VERSION 3.24)
project(RemoteManagerUiPreview LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Widgets Test)
add_library(ui_preview STATIC PreviewWindow.cpp PreviewMenus.cpp PreviewWindow.h
    ../ProfileEditor.cpp ../ProfileEditor.h ../PropertyDelegate.cpp ../PropertyDelegate.h
    ../ConnectionDefaults.cpp ../ConnectionDefaults.h ../ConnectionIcons.cpp ../ConnectionIcons.h
    ../Secret.cpp ../Secret.h ../PasswordEdit.h
    ../ConnectionTree.cpp ../ConnectionTree.h
    ../Branding.cpp ../Branding.h ../ui/branding.qrc
    ../ui/test.ui ../ui/preview-icons.qrc)
set_source_files_properties(PreviewWindow.cpp PreviewMenus.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ui/test.ui")
target_include_directories(ui_preview PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ui_preview PUBLIC Qt6::Widgets)
add_executable(UiPreview main.cpp)
target_link_libraries(UiPreview PRIVATE ui_preview)
target_compile_definitions(UiPreview PRIVATE RM_PREVIEW_OUTPUT_DIR="${CMAKE_BINARY_DIR}")
add_executable(ui_preview_test preview_test.cpp)
target_link_libraries(ui_preview_test PRIVATE ui_preview Qt6::Test)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(ui_preview_test PRIVATE NativeMenuTest.mm)
    target_link_libraries(ui_preview_test PRIVATE "-framework AppKit")
endif()
enable_testing()
add_test(NAME ui_preview_test COMMAND ui_preview_test)
set_tests_properties(ui_preview_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
