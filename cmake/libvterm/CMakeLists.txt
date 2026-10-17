cmake_minimum_required(VERSION 3.24)
project(VTerm VERSION 0.3.3 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT EXISTS "${RM_LIBVTERM_SOURCE}/include/vterm.h")
    message(FATAL_ERROR "RM_LIBVTERM_SOURCE must name the verified libvterm-0.3.3 source")
endif()
add_library(vterm STATIC
    "${RM_LIBVTERM_SOURCE}/src/encoding.c"
    "${RM_LIBVTERM_SOURCE}/src/keyboard.c"
    "${RM_LIBVTERM_SOURCE}/src/mouse.c"
    "${RM_LIBVTERM_SOURCE}/src/parser.c"
    "${RM_LIBVTERM_SOURCE}/src/pen.c"
    "${RM_LIBVTERM_SOURCE}/src/screen.c"
    "${RM_LIBVTERM_SOURCE}/src/state.c"
    "${RM_LIBVTERM_SOURCE}/src/unicode.c"
    "${RM_LIBVTERM_SOURCE}/src/vterm.c")
set_target_properties(vterm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(vterm PUBLIC
    "$<BUILD_INTERFACE:${RM_LIBVTERM_SOURCE}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
install(TARGETS vterm EXPORT VTermTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${RM_LIBVTERM_SOURCE}/include/vterm.h" "${RM_LIBVTERM_SOURCE}/include/vterm_keycodes.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${RM_LIBVTERM_SOURCE}/LICENSE" DESTINATION share/licenses/libvterm)
install(EXPORT VTermTargets NAMESPACE VTerm:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/VTerm")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/VTermConfig.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/VTermTargets.cmake\")\n")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/VTermConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VTermConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/VTermConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/VTerm")
