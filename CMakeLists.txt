cmake_minimum_required(VERSION 3.22)
# quickjs-ng and pocketjs_guest are vendored under components/ (default search
# path, no EXTRA_COMPONENT_DIRS needed). Every app draws through Kasane
# (main/ui/kasane); the Rust UI core, its QuickJS binding and the rgb565
# renderer are no longer part of the image, so nothing is built out of the
# pinned .cache/pocketjs checkout.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_pocketjs)
# lwIP's DNS and ARP tables (docs/platform/lwip-resource-review.md K3). No
# Kconfig for either and lwipopts.h leaves both to opt.h's #ifndef defaults
# (4 and 10), which only dns.c and etharp.c read. A station that asks for one
# NTP host and one HTTP host at a time, behind one gateway, needs 2 and 4:
# -552 B and -144 B of .bss.
idf_component_get_property(_lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${_lwip_lib} PRIVATE DNS_TABLE_SIZE=2 ARP_TABLE_SIZE=4)
# Profile-guided QuickJS (docs/vm/qjs-pgo.md), off by default.
include("${CMAKE_SOURCE_DIR}/tools/pgo/qjs_pgo.cmake")
if(CONFIG_POCKET_VM_QJS_PGO)
    pocket_qjs_pgo(use "${CMAKE_SOURCE_DIR}/tools/pgo/qjs")
endif()
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
# Stack accesses beyond the 1,020-byte reach of l32i/s32i: each costs 1-2 extra
# instructions (docs/perf/pie-simd.md 3.9). Report only for now, over main/'s
# objects; a function that gains far accesses against tools/frame_lint_baseline.txt
# is marked NEW or GREW.
add_custom_target(frame_lint ALL
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/frame_lint.py"
            "--objdump=${CMAKE_OBJDUMP}" --far-only
            --baseline "${CMAKE_SOURCE_DIR}/tools/frame_lint_baseline.txt"
            "${CMAKE_BINARY_DIR}/esp-idf/main"
    DEPENDS gen_project_binary VERBATIM)
