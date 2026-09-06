cmake_minimum_required(VERSION 3.22)
set(POCKETJS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/.cache/pocketjs" CACHE PATH "Pinned PocketJS checkout")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/.cache/components/pocketjs_guest")
foreach(component pocketjs_ui_core pocketjs_ui_qjs pocketjs_render_rgb565)
    list(APPEND EXTRA_COMPONENT_DIRS "${POCKETJS_SOURCE_DIR}/hosts/esp-idf/components/${component}")
endforeach()
set(POCKETJS_UI_CORE_ARCHIVE "${CMAKE_SOURCE_DIR}/.cache/native/libpocketjs_idf_ui_core.a" CACHE FILEPATH "S3 UI archive")
set(POCKETJS_RENDER_RGB565_ARCHIVE "${CMAKE_SOURCE_DIR}/.cache/native/libpocketjs_idf_render_rgb565.a" CACHE FILEPATH "S3 renderer archive")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_pocketjs)
add_custom_target(flash_budget ALL
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/check_flash.py" "${CMAKE_BINARY_DIR}/cardputer_pocketjs.bin"
    DEPENDS gen_project_binary VERBATIM)
