# Profile-guided build of the QuickJS component (docs/vm/qjs-pgo.md).
#
#   pocket_qjs_pgo(generate <unused>)  instrument; the image dumps its counters
#                                      (tools/r5sim, R5SIM_PGO_GEN)
#   pocket_qjs_pgo(use <profile dir>)  compile with the profile in <profile dir>
#
# Call it from a project CMakeLists after project(). The profile dir mirrors
# the component's sources: <dir>/quickjs-ng/quickjs.c.gcda and so on.
#
# Why the files are copied next to the objects: a bare -fprofile-use looks for
# <object minus .obj>.gcda, a path relative to the build directory that is the
# same on every machine. -fprofile-use=DIR instead mangles the absolute object
# path into the name, which differs between this Linux container and a Windows
# build tree.
#
# Why -Wno-coverage-mismatch: GCC's per-function check includes the source
# file's path as given on the command line, so a profile trained in one tree
# fails it in any other ("source locations ... have changed", an error by
# default). GCC still uses the counts in that case (checked: the same object
# code as a build from the training tree). A function whose control flow
# really changed fails a separate checksum and gets no profile -- silently
# here. -Wmissing-profile stays on as a warning (see below).

# Code generation for both builds (docs/vm/qjs-pgo.md). The component stays at
# -Os.
# - stc: lay the blocks out as traces of the profile's hot paths, with the
#   blocks the training never ran after them (-Os otherwise picks "simple",
#   which keeps the source order).
# - no-reorder-functions: a profile would otherwise move functions into
#   .text.hot.<name> / .text.unlikely.<name>, and main/vm/helper_iram8.lf,
#   which maps the 93 IRAM helpers by their .text.<name> section, would no
#   longer find them -- they would stay in flash without a word.
# Not available: hot/cold splitting (-freorder-blocks-and-partition). The
# Xtensa GCC turns it off whatever the options say (esp-15.2.0 reports it
# [disabled]; the same test splits on x86).
if(NOT DEFINED POCKET_QJS_PGO_OPTS)
    set(POCKET_QJS_PGO_OPTS -freorder-blocks-algorithm=stc -fno-reorder-functions)
endif()

function(pocket_qjs_pgo mode profile_dir)
    idf_component_get_property(qjs_lib quickjs-ng COMPONENT_LIB)
    idf_component_get_property(qjs_dir quickjs-ng COMPONENT_DIR)
    if(mode STREQUAL "generate")
        # single: 64-bit counters without atomics (one core runs the guest).
        # info-section: the image walks the gcov_info records itself
        # (tools/r5sim/main/gcov_info.lf) instead of writing files.
        # The same code-generation options as "use": the profile is read after
        # the early inliner, so both builds must inline alike or the counts do
        # not fit the functions ("Missing counts for called function").
        target_compile_options(${qjs_lib} PRIVATE
            ${POCKET_QJS_PGO_OPTS}
            -fprofile-generate -fprofile-update=single -fprofile-info-section)
        target_link_libraries(${qjs_lib} INTERFACE gcov)
    elseif(mode STREQUAL "use")
        if(NOT EXISTS "${profile_dir}/quickjs-ng/quickjs.c.gcda")
            message(FATAL_ERROR "QJS PGO: no profile in ${profile_dir} (tools/pgo/train.sh)")
        endif()
        set(obj_dir "${CMAKE_BINARY_DIR}/esp-idf/quickjs-ng/CMakeFiles/${qjs_lib}.dir")
        file(GLOB_RECURSE gcdas RELATIVE "${profile_dir}" "${profile_dir}/*.gcda")
        foreach(g ${gcdas})
            configure_file("${profile_dir}/${g}" "${obj_dir}/${g}" COPYONLY)
            string(REGEX REPLACE "\\.gcda$" "" src "${g}")
            # Per source: a file the training image did not link (port/stub.c,
            # quickjs-libc.c) has no profile and builds as before.
            # -Wmissing-profile reports a .gcda that is not found (once per
            # file) and a function the training image did not have (the
            # firmware's CONFIG_POCKET_VM_ROM_INTRINSICS setup, a handful);
            # a warning, not the IDF's -Werror: those build without a profile.
            set_property(SOURCE "${qjs_dir}/${src}" TARGET_DIRECTORY ${qjs_lib}
                APPEND PROPERTY COMPILE_OPTIONS ${POCKET_QJS_PGO_OPTS}
                    -fprofile-use -Wno-coverage-mismatch -Wmissing-profile
                    -Wno-error=missing-profile)
            # A new profile recompiles its source.
            set_property(SOURCE "${qjs_dir}/${src}" TARGET_DIRECTORY ${qjs_lib}
                APPEND PROPERTY OBJECT_DEPENDS "${profile_dir}/${g}")
        endforeach()
    else()
        message(FATAL_ERROR "pocket_qjs_pgo: mode is generate or use, not '${mode}'")
    endif()
endfunction()
