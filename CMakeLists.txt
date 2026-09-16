cmake_minimum_required(VERSION 3.22)
# quickjs-ng and pocketjs_guest are vendored under components/ (default search
# path, no EXTRA_COMPONENT_DIRS needed). Every app draws through Kasane
# (main/ui/kasane); the Rust UI core, its QuickJS binding and the rgb565
# renderer are no longer part of the image, so nothing is built out of the
# pinned .cache/pocketjs checkout.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_pocketjs)
add_custom_target(flash_budget ALL
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/check_flash.py" "${CMAKE_BINARY_DIR}/cardputer_pocketjs.bin"
    DEPENDS gen_project_binary VERBATIM)
# DRAM is the scarcer budget on a part with no PSRAM, and until this existed
# nobody was adding up what each capability took. Static only -- reading the
# device needs a serial port and a build must not want one -- and it writes
# nothing when a rebuild moved no memory, so the log is a list of the changes
# rather than of the builds.
add_custom_target(memory_log ALL
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/memlog.py"
            --map "${CMAKE_BINARY_DIR}/cardputer_pocketjs.map" --only-changes
    DEPENDS gen_project_binary VERBATIM)
