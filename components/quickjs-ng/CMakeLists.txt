# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Applications include only quickjs.h / quickjs-libc.h from INCLUDE_DIRS; port/include is
# for compiling this component only (poll.h, esp_libc_compat, etc.).

idf_component_register(
    SRCS
        "quickjs-ng/dtoa.c"
        "quickjs-ng/libregexp.c"
        "quickjs-ng/libunicode.c"
        "quickjs-ng/quickjs.c"
        "quickjs-ng/quickjs-libc.c"
        "quickjs-ng/quickjs-vm.c"
        "port/stub.c"
    INCLUDE_DIRS
        "quickjs-ng"
    PRIV_INCLUDE_DIRS
        "port/include"
    PRIV_REQUIRES
        vfs
)

# QJS_DISABLE_DUMPS: no JS_DUMP_* hooks (JS_SetDumpFlags becomes a no-op)
# while assert() stays on. Without it, assertions-enabled builds (no NDEBUG)
# get ENABLE_DUMPS and a dump-flag test on every opcode dispatch
# (docs/vm/interpreter-stages.md sec.4.1).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    QUICKJS_NG_BUILD
    QJS_DISABLE_DUMPS
    _GNU_SOURCE
)

# Measurement build (main/Kconfig.projbuild POCKET_GC_STATS): the firmware
# supplies pocket_gc_clock and reads JS_PocketGCStats / JS_PocketGCTime.
if(CONFIG_POCKET_GC_STATS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKET_GC_STATS=1)
endif()

# Experimental hot/cold layout of quickjs.c (docs/vm/qjs-hotcold.md).
# main/Kconfig.projbuild POCKET_VM_QJS_HOTCOLD (default n) lays the basic
# blocks of its hot functions out by tools/qjs_hotcold/profile.json. Not a
# line of QuickJS changes: tools/qjs_hotcold/hc_cc.py stands in front of the
# compiler for this component, and for quickjs.c alone compiles to assembly,
# moves the blocks (hc_asm.py, which also verifies the result against GCC's
# output), and assembles that.
#   -DPOCKET_QJS_HOTCOLD=use|gen   the same without the Kconfig (tools/r5sim
#       has none); `gen` only adds a symbol per block, for training
#   -DPOCKET_QJS_HOTCOLD_PROFILE=<file>   another profile
#   -DPOCKET_QJS_HOTCOLD_PAD=<bytes>   shift the hot code (measurement)
# A device A/B through a builder that only takes the default Kconfig: a
# `measure:` commit sets the next line to "use" (and reverts it afterwards).
set(_qjs_hc "use")
set(_qjs_hc_pad 0)
if(POCKET_QJS_HOTCOLD)
    set(_qjs_hc "${POCKET_QJS_HOTCOLD}")
elseif(CONFIG_POCKET_VM_QJS_HOTCOLD)
    set(_qjs_hc "use")
endif()
if(POCKET_QJS_HOTCOLD_PAD)
    set(_qjs_hc_pad "${POCKET_QJS_HOTCOLD_PAD}")
endif()
if(_qjs_hc)
    get_filename_component(_qjs_hc_dir "${CMAKE_CURRENT_LIST_DIR}/../../tools/qjs_hotcold" ABSOLUTE)
    if(NOT POCKET_QJS_HOTCOLD_PROFILE)
        set(POCKET_QJS_HOTCOLD_PROFILE "${_qjs_hc_dir}/profile.json")
    endif()
    get_filename_component(_qjs_hc_profile "${POCKET_QJS_HOTCOLD_PROFILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    idf_build_get_property(_qjs_hc_python PYTHON)
    set_property(TARGET ${COMPONENT_LIB} PROPERTY C_COMPILER_LAUNCHER
        "${_qjs_hc_python}" -B "${_qjs_hc_dir}/hc_cc.py" --mode "${_qjs_hc}" --profile "${_qjs_hc_profile}"
        --pad "${_qjs_hc_pad}" --keep-out "${CMAKE_CURRENT_LIST_DIR}/../../main/vm/helper_iram8.lf"
        --only quickjs.c --)
    set(_qjs_hc_deps "${_qjs_hc_dir}/hc_cc.py" "${_qjs_hc_dir}/hc_asm.py")
    if(_qjs_hc STREQUAL "use")
        list(APPEND _qjs_hc_deps "${_qjs_hc_profile}")
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/quickjs.c" APPEND PROPERTY OBJECT_DEPENDS ${_qjs_hc_deps})
    message(STATUS "quickjs-ng: hot/cold layout '${_qjs_hc}' (${_qjs_hc_profile})")
endif()

# Third-party source files: suppress warnings that are noisy but harmless in upstream code.
# -Wincompatible-pointer-types stays on (an error in GCC 14+): an int * handed to
# an int32_t * parameter is a strict-aliasing miscompile on Xtensa, where
# int32_t is long (docs/vm/aliasing-types.md). Only quickjs-libc.c keeps it off:
# its 16 sites are in the std/os modules, which the firmware never registers
# (none of them is in the ELF).
set(_qjs_vendor_warnings
    -Wno-pointer-sign
    -Wno-cast-function-type
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-type-limits
    -Wno-implicit-fallthrough
    -Wno-format
    -Wno-strict-aliasing
    -Wno-empty-body
)

# Apply vendor warnings only to upstream QuickJS files (not port/stub.c)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/dtoa.c"
    PROPERTIES COMPILE_OPTIONS "${_qjs_vendor_warnings};-Wno-uninitialized;-Wno-maybe-uninitialized"
)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/libregexp.c"
    PROPERTIES COMPILE_OPTIONS "${_qjs_vendor_warnings}"
)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/libunicode.c"
    PROPERTIES COMPILE_OPTIONS "${_qjs_vendor_warnings}"
)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/quickjs.c"
    PROPERTIES COMPILE_OPTIONS "${_qjs_vendor_warnings};-Wno-maybe-uninitialized"
)

# quickjs-libc.c: ESP_PLATFORM feature gates for unsupported host APIs; esp_libc_compat.h for lstat and newlib.
set(_qjs_libc "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng/quickjs-libc.c")
set(_qjs_libc_compat "${CMAKE_CURRENT_SOURCE_DIR}/port/include/esp_libc_compat.h")
set_source_files_properties(${_qjs_libc}
    PROPERTIES
        COMPILE_OPTIONS "-include;${_qjs_libc_compat};${_qjs_vendor_warnings};-Wno-incompatible-pointer-types"
)

# newlib: poll and related declarations (quickjs-libc.c includes <poll.h> via port/include/poll.h).
idf_build_get_property(_qjs_idf_path IDF_PATH)
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    "${_qjs_idf_path}/components/newlib/platform_include"
)
