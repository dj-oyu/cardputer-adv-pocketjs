# The hot/cold layout's compiler launcher, for any component
# (docs/vm/qjs-hotcold.md; quickjs-ng and main use it).
#
#   pocket_hotcold(MODE use|gen PROFILE <json> [PAD <bytes>]
#                  ONLY <basename>... SOURCES <path>... [KEEP_OUT <lf>...] [NAME <label>])
#
# Puts tools/qjs_hotcold/hc_cc.py in front of the C compiler of
# ${COMPONENT_LIB}. hc_cc.py lays out only the files named by ONLY (source
# basenames) and passes every other compile straight through. SOURCES are the
# same files as paths: they get the tools and the profile as dependencies, so
# a new profile recompiles them. KEEP_OUT: linker fragments whose functions,
# placed by name, must stay in their own sections.
get_filename_component(POCKET_HOTCOLD_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

function(pocket_hotcold)
    cmake_parse_arguments(HC "" "MODE;PROFILE;PAD;NAME" "ONLY;SOURCES;KEEP_OUT" ${ARGN})
    if(NOT HC_PAD)
        set(HC_PAD 0)
    endif()
    get_filename_component(_profile "${HC_PROFILE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    idf_build_get_property(_python PYTHON)
    list(JOIN HC_ONLY "," _only)
    set(_keep "")
    if(HC_KEEP_OUT)
        list(JOIN HC_KEEP_OUT "," _k)
        set(_keep --keep-out "${_k}")
    endif()
    # -B: the build must not write a __pycache__ into the source tree.
    set_property(TARGET ${COMPONENT_LIB} PROPERTY C_COMPILER_LAUNCHER
        "${_python}" -B "${POCKET_HOTCOLD_DIR}/hc_cc.py" --mode "${HC_MODE}" --profile "${_profile}"
        --pad "${HC_PAD}" ${_keep} --only "${_only}" --)
    set(_deps "${POCKET_HOTCOLD_DIR}/hc_cc.py" "${POCKET_HOTCOLD_DIR}/hc_asm.py")
    if(HC_MODE STREQUAL "use")
        list(APPEND _deps "${_profile}")
    endif()
    set_property(SOURCE ${HC_SOURCES} APPEND PROPERTY OBJECT_DEPENDS ${_deps})
    message(STATUS "${HC_NAME}: hot/cold layout '${HC_MODE}' of ${_only} (${_profile})")
endfunction()

# main/'s files the native layout covers (paths under main/): the ones whose
# never-run bytes sit in a turn's touched lines, most first (r5sim, STRESS and
# the DERBY demo race, docs/vm/qjs-hotcold.md sec.13). Shared by
# main/CMakeLists.txt and tools/r5sim/main/CMakeLists.txt, so the image that
# trains and the firmware lay out the same files.
set(POCKET_NATIVE_HOTCOLD_FILES
    ui/kasane/ksn_shiki.c pocket/pocket_kasane.c ui/kasane/ksn_core.c pocket/pocket_shiki_frame.c
    pocket/pocket_shiki_api.c pocket/derby_ser_int.c pocket/pocket_derby.c ui/kasane/ksn_view.c
    pocket/pocket_input.c)

# pocket_native_hotcold(<main dir>): the launcher on main's sources, when
# CONFIG_POCKET_NATIVE_HOTCOLD or -DPOCKET_NATIVE_HOTCOLD=use|gen asks for it
# (-DPOCKET_NATIVE_HOTCOLD=off overrides the Kconfig).
function(pocket_native_hotcold main_dir)
    set(_mode "")
    if(DEFINED POCKET_NATIVE_HOTCOLD)
        if(POCKET_NATIVE_HOTCOLD STREQUAL "use" OR POCKET_NATIVE_HOTCOLD STREQUAL "gen")
            set(_mode "${POCKET_NATIVE_HOTCOLD}")
        endif()
    elseif(CONFIG_POCKET_NATIVE_HOTCOLD)
        set(_mode "use")
    endif()
    if(NOT _mode)
        return()
    endif()
    if(NOT POCKET_NATIVE_HOTCOLD_PROFILE)
        set(POCKET_NATIVE_HOTCOLD_PROFILE "${POCKET_HOTCOLD_DIR}/profile_native.json")
    endif()
    set(_only "")
    set(_paths "")
    foreach(f ${POCKET_NATIVE_HOTCOLD_FILES})
        get_filename_component(_b "${f}" NAME)
        list(APPEND _only "${_b}")
        list(APPEND _paths "${main_dir}/${f}")
    endforeach()
    pocket_hotcold(NAME main MODE "${_mode}" PROFILE "${POCKET_NATIVE_HOTCOLD_PROFILE}"
        PAD "${POCKET_NATIVE_HOTCOLD_PAD}" ONLY ${_only} SOURCES ${_paths}
        KEEP_OUT "${main_dir}/vm/helper_iram8.lf")
endfunction()
