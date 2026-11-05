# R5's offline instruction-cache model (docs/vm/r5-icache.md): STRESS's JS
# turns on an ESP32-S3 image that QEMU can run -- UART console, no display,
# no keyboard -- built with the firmware's compiler settings, so every function
# the turn runs is byte-for-byte the firmware's and a trace of it can be laid
# onto the firmware's own addresses (tools/r5sim/icache.py).
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/quickjs-ng")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(r5sim)
# The firmware's QuickJS options that live in main/Kconfig.projbuild, which this
# project does not have: CONFIG_POCKET_VM_ROM_INTRINSICS (default y) is the
# only one quickjs.c reads. Without it the image built different context
# setup code than the firmware (and the PGO profile missed those functions).
idf_component_get_property(_qjs_lib quickjs-ng COMPONENT_LIB)
target_compile_definitions(${_qjs_lib} PRIVATE CONFIG_POCKET_VM_ROM_INTRINSICS=1)
# Profile-guided QuickJS (docs/vm/qjs-pgo.md): -DR5SIM_PGO=generate trains,
# -DR5SIM_PGO=use builds with tools/pgo/qjs (to model the result).
include("${CMAKE_CURRENT_LIST_DIR}/../pgo/qjs_pgo.cmake")
if(R5SIM_PGO)
    pocket_qjs_pgo(${R5SIM_PGO} "${CMAKE_CURRENT_LIST_DIR}/../pgo/qjs")
    if(CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND)
        # PSRAM (sdkconfig.pgo_gen.defaults) turns on -mdisable-hardware-atomics
        # for every file, which would change QuickJS's object code (and fail its
        # lock-free assert). Its atomics are all in DRAM; the counters in PSRAM
        # are not atomic. Undone for the profiled component only.
        target_compile_options(${_qjs_lib} PRIVATE -mno-disable-hardware-atomics)
    endif()
endif()
