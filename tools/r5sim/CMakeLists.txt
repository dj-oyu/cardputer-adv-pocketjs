# R5's offline instruction-cache model (docs/vm/turn-icache.md sec.1): STRESS's JS
# turns on an ESP32-S3 image that QEMU can run -- UART console, no display,
# no keyboard -- built with the firmware's compiler settings, so every function
# the turn runs is byte-for-byte the firmware's and a trace of it can be laid
# onto the firmware's own addresses (tools/r5sim/icache.py).
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/quickjs-ng"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/pocketjs_guest")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# Allocator experiments (docs/vm/qjs-observe.md sec.10): the R3 cache's depth
# for every component, and with -DR5SIM_GUEST_ALLOC=1 its size classes.
# -DR5SIM_CHUNK_KIB=16: the growable arena (docs/vm/guest-memory-design.md sec.5).
if(DEFINED R5SIM_CHUNK_KIB)
    math(EXPR _r5_chunk "${R5SIM_CHUNK_KIB} * 1024")
    idf_build_set_property(COMPILE_DEFINITIONS "POCKET_GUEST_CHUNK_BYTES=${_r5_chunk}U" APPEND)
endif()
# -DR5SIM_MEMUSAGE=1: JS_ComputeMemoryUsage after each evaluation, before and after a GC.
if(R5SIM_MEMUSAGE)
    idf_build_set_property(COMPILE_DEFINITIONS "R5SIM_MEMUSAGE=1" APPEND)
endif()
if(R5SIM_CHUNK_TRACE)
    idf_build_set_property(COMPILE_DEFINITIONS "POCKET_GUEST_CHUNK_TRACE=1;R5SIM_CHUNK_TRACE=1" APPEND)
endif()
if(DEFINED R5SIM_ARENA_KIB)
    math(EXPR _r5_arena "${R5SIM_ARENA_KIB} * 1024")
    idf_build_set_property(COMPILE_DEFINITIONS "POCKET_GUEST_ARENA_BYTES=${_r5_arena}U" APPEND)
endif()
# The checkout's path out of the image: ESP-IDF maps only this project's
# directory and its own in __FILE__, so main/'s and components/' asserts kept
# the absolute path in .flash.rodata, and the counts moved with the checkout
# (154,595 at the main checkout, 154,611 in a worktree, the same commit:
# docs/kasane/logs/api-reorg-perf.md). -DR5SIM_PATH_MAP=0 keeps the old image.
get_filename_component(_r5_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT DEFINED R5SIM_PATH_MAP OR R5SIM_PATH_MAP)
    idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${_r5_root}=." APPEND)
endif()
# -DR5SIM_DEVICE=1: guest.c's read of its allocation counters (R5SIM alloc).
if(R5SIM_DEVICE)
    idf_build_set_property(COMPILE_DEFINITIONS "POCKET_GUEST_ALLOC_COUNTS=1" APPEND)
endif()
if(R5SIM_CACHE_DEPTH)
    idf_build_set_property(COMPILE_DEFINITIONS "BLOCK_CACHE_DEPTH=${R5SIM_CACHE_DEPTH}" APPEND)
endif()
project(r5sim)
# The firmware's QuickJS options that live in main/Kconfig.projbuild, which this
# project does not have: CONFIG_POCKET_VM_ROM_INTRINSICS (default y) is the
# only one quickjs.c reads. Without it the image built different context
# setup code than the firmware (and the PGO profile missed those functions).
idf_component_get_property(_qjs_lib quickjs-ng COMPONENT_LIB)
target_compile_definitions(${_qjs_lib} PRIVATE CONFIG_POCKET_VM_ROM_INTRINSICS=1)
# -DR5SIM_GC_TRACE=1: QuickJS's collection counters (JS_PocketGCStats) for the
# per-turn heap line (docs/vm/lv3-heap-exhaustion.md).
if(R5SIM_GC_TRACE)
    target_compile_definitions(${_qjs_lib} PRIVATE POCKET_GC_STATS=1)
endif()
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
