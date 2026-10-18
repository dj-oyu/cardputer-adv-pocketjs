cmake_minimum_required(VERSION 3.22)
# Explicit diagnostic source revision: ordinary builds of this revision select
# telemetry without caller flags. The normal PR8 branch keeps its default OFF.
set(POCKET_DERBY_TELEMETRY ON CACHE BOOL "Explicit DERBY diagnostic source revision")
# quickjs-ng and pocketjs_guest are vendored under components/ (default search
# path, no EXTRA_COMPONENT_DIRS needed). Every app draws through Kasane
# (main/ui/kasane); the Rust UI core, its QuickJS binding and the rgb565
# renderer are no longer part of the image, so nothing is built out of the
# pinned .cache/pocketjs checkout.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_pocketjs)
# This explicit diagnostic revision must prove ON in the actual output even
# when built by the broker's fixed argv from a source archive without .git.
add_custom_target(diagnostic_mode_proof ALL
    COMMAND "${PYTHON}" -B "${CMAKE_SOURCE_DIR}/tools/verify_diagnostic_build.py"
            --build "${CMAKE_BINARY_DIR}"
    DEPENDS gen_project_binary VERBATIM)
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
            --map "${CMAKE_BINARY_DIR}/cardputer_pocketjs.map"
            --log "${CMAKE_BINARY_DIR}/memory.jsonl" --only-changes
    DEPENDS gen_project_binary VERBATIM)
