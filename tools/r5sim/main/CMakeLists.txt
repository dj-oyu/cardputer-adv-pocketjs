# The same sources tools/build_kasane_test.sh gives tools/test_stress_app.c on
# the host, compiled for the chip. tools/hostshim stays off the include path:
# its esp_log.h and friends would shadow ESP-IDF's.
set(R "${CMAKE_CURRENT_LIST_DIR}/../../..")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
execute_process(COMMAND "${PYTHON}" "${R}/tools/make_font.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../pet_header.py"
    "${R}/apps/pet/assets/pets-compact.bin" "${CMAKE_BINARY_DIR}/generated/kasane_pet_test_data.h"
    COMMAND_ERROR_IS_FATAL ANY)
# The workload (docs/kasane/api-reorg.md sec.6 P0):
#   (unset)  the R5 records' Kasane-retained STRESS (stress_r5.js), no Shiki.
#            docs/vm/qjs-observe.md sec.12.
#   shiki    today's STRESS (apps/stress/stress.js, drawn with
#            pocket.kasane.shiki), as tools/build_stress_app_test.sh builds it
#            on the host: the app's Shiki programs become the built-in table
#            (Node 16+).
#            (A frozen copy of the pre-reorg STRESS, "shiki_v1", ran here until
#            api-reorg P5 removed the calls it exercised.)
#   derby    DERBY WATCH's demo race (apps/derby, its chunks lowered and its
#            @shiki programs compiled as main/CMakeLists.txt does), with
#            pocket.derby, pocket.app.load and pocket.input; counts per scene.
#   hud      LCD CATCH (apps/lcdcatch): retained mode only (replace/patch,
#            refs, cache), no Shiki; pocket.input fed by a bot.
set(R5SIM_APP_SOURCES "")
set(R5SIM_APP_JS "${CMAKE_CURRENT_LIST_DIR}/stress_r5.js")
set(R5SIM_SHIKI_SOURCES "")
set(R5SIM_CHUNK_FILES "")
set(R5SIM_SHIKI_ROM "")
if(R5SIM_APP STREQUAL "shiki")
    set(R5SIM_SHIKI_ROM "stress=apps/stress/stress_prog.js")
    set(R5SIM_APP_JS "${R}/apps/stress/stress.js")
elseif(R5SIM_APP STREQUAL "derby")
    # pocket.app.load()'s table for local.derby alone, as main/CMakeLists.txt
    # makes it; the prog chunk without its @shiki functions (lower_plans.mjs),
    # done here at configure time (this image has no other chunk to rebuild).
    find_program(R5SIM_NODE node REQUIRED)
    set(_g "${CMAKE_BINARY_DIR}/generated/derby")
    execute_process(COMMAND "${PYTHON}" "${R}/tools/make_app_chunks.py" "${_g}" "${R}/apps/derby/chunks.txt"
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    include("${_g}/app_chunks.cmake")
    set(_lower ${APP_CHUNK_LOWER})
    while(_lower)
        list(POP_FRONT _lower _src _dst _how)
        string(REGEX REPLACE "^rom:" "" _app "${_how}")
        execute_process(COMMAND "${R5SIM_NODE}" "${R}/tools/kasane_ir/lower_plans.mjs" --rom "${_app}"
            --file "${_src}" "${_dst}" WORKING_DIRECTORY "${R}" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    endwhile()
    set(R5SIM_SHIKI_ROM ${APP_CHUNK_SHIKI})
    set(R5SIM_CHUNK_FILES ${APP_CHUNK_FILES})
    set(R5SIM_APP_JS "${R}/apps/derby/derby_watch.js")
    set(R5SIM_APP_SOURCES "${_g}/app_chunks.c" "${R}/main/pocket/pocket_app_load.c"
        "${R}/main/pocket/app_registry.c" "${R}/main/pocket/pocket_derby.c" "${R}/main/pocket/shiki_memstat.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${R}/apps/derby/chunks.txt"
        "${R}/apps/derby/derby_prog.js")
elseif(R5SIM_APP STREQUAL "hud")
    set(R5SIM_APP_JS "${R}/apps/lcdcatch/lcd_catch.js")
elseif(R5SIM_APP)
    message(FATAL_ERROR "R5SIM_APP=${R5SIM_APP}: one of shiki, derby, hud (or unset)")
endif()
if(R5SIM_SHIKI_ROM)
    find_program(R5SIM_NODE node REQUIRED)
    execute_process(COMMAND "${R5SIM_NODE}" "${R}/tools/kasane_ir/emit_shiki_rom.mjs"
        "${CMAKE_BINARY_DIR}/generated/stress_shiki_rom.c" ${R5SIM_SHIKI_ROM}
        WORKING_DIRECTORY "${R}" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    set(R5SIM_SHIKI_SOURCES "${R}/main/pocket/pocket_shiki_frame.c" "${R}/main/pocket/shiki_heap.c"
        "${R}/main/ui/kasane/ksn_shiki.c" "${R}/main/ui/kasane/ksn_shiki_frame.c"
        "${R}/main/pocket/pocket_shiki_api.c"
        # pocket_shiki_frame.c draws opaque mesh depth (beginMeshFrame).
        "${R}/main/ui/kasane/ksn_mesh_depth.c" "${R}/main/ayatsuri/compact_opaque.c")
    list(APPEND R5SIM_APP_SOURCES ${R5SIM_SHIKI_SOURCES} "${CMAKE_BINARY_DIR}/generated/stress_shiki_rom.c")
endif()
if(R5SIM_APP STREQUAL "derby" OR R5SIM_APP STREQUAL "hud")
    # pocket.input.keys, real (keymap + keystate), with pocket_api.c's
    # subscription code verbatim as the host harnesses take it
    # (tools/games/run_lcd_catch.sh): r5sim.c includes pocket_sub_impl.inc.
    file(READ "${R}/main/pocket/pocket_api.c" _api)
    string(FIND "${_api}" "// ----------------------------------------------------------- subscriptions" _a)
    string(FIND "${_api}" "// ------------------------------------------------------- async completions" _b)
    if(_a LESS 0 OR _b LESS _a)
        message(FATAL_ERROR "pocket_api.c: the subscriptions section moved; update tools/r5sim/main/CMakeLists.txt")
    endif()
    math(EXPR _n "${_b} - ${_a}")
    string(SUBSTRING "${_api}" ${_a} ${_n} _sub)
    file(WRITE "${CMAKE_BINARY_DIR}/generated/pocket_sub_impl.inc" "${_sub}")
    list(APPEND R5SIM_APP_SOURCES "${R}/main/pocket/pocket_input.c" "${R}/main/hal/keymap.c"
        "${R}/main/hal/keystate.c")
endif()
set(R5SIM_LDFRAGMENTS "")
# R5SIM_HELPER_IRAM=0 leaves them in flash, to choose the list again from a
# trace (tools/r5sim/icache.py --iram-kib; docs/vm/qjs-observe.md sec.14).
if(R5SIM_DEVICE AND NOT (DEFINED R5SIM_HELPER_IRAM AND NOT R5SIM_HELPER_IRAM))
    # The firmware's CONFIG_POCKET_VM_HELPER_IRAM=y (default): its QuickJS
    # helpers in IRAM, so the trace runs them from where the device does.
    list(APPEND R5SIM_LDFRAGMENTS "${R}/main/vm/helper_iram8.lf")
endif()
if(R5SIM_PGO STREQUAL "generate")
    list(APPEND R5SIM_LDFRAGMENTS "gcov_info.lf")
endif()
idf_component_register(
    SRCS "r5sim.c"
        "${R}/tools/hostshim/pocket_api_stub.c" "${R}/tools/hostshim/jpfont.c"
        "${R}/tools/hostshim/ksn_pet_builtin.c"
        "${R}/main/text/ksn_font.c" "${R}/main/pet/ksn_pet.c" "${R}/main/pet/pet_pixels.c"
        "${R}/main/pocket/pocket_memory.c"
        "${R}/main/pocket/pocket_clock_source.c" "${R}/main/pocket/pocket_clock.c"
        "${R}/main/pocket/pocket_av_playback_source.c" "${R}/main/pocket/pocket_av_output_source.c"
        "${R}/main/pocket/app_view_assets.c" "${R}/main/pocket/app_view_provider.c"
        "${R}/main/pocket/app_music_view.c" "${R}/main/pocket/app_legacy_presenter.c"
        "${R}/main/pocket/pocket_kasane.c" "${R}/main/pocket/app_notice.c"
        "${R}/main/ui/kasane/ksn_runtime.c" "${R}/main/ui/kasane/ksn_schema.c"
        "${R}/main/ui/kasane/ksn_schema_session.c" "${R}/main/ui/kasane/ksn_source.c"
        "${R}/main/ui/kasane/ksn_source_pool.c" "${R}/main/ui/kasane/ksn_source_pool_adapter.c"
        "${R}/main/ui/kasane/ksn_core.c" "${R}/main/ui/kasane/ksn_view.c"
        "${R}/main/ui/kasane/ksn_cache.c" "${R}/main/ui/kasane/ksn_modal.c"
        "${R}/main/ui/kasane/ksn_render.c" "${R}/main/ui/kasane/ksn_blend_pie.c"
        "${R}/main/hal/disp.c"
        ${R5SIM_APP_SOURCES}
    INCLUDE_DIRS "." "${R}/main" "${R}/components/pocketjs_guest/include" "${R}/main/pocket" "${R}/main/ui" "${R}/main/ui/kasane"
        "${R}/main/text" "${R}/main/hal" "${CMAKE_BINARY_DIR}/generated"
    REQUIRES quickjs-ng pocketjs_guest esp_timer esp_hw_support log esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_i2s
    # stress_r5.js is apps/stress/stress.js as of 51f5feb, byte for byte (the
    # Kasane retained scene, before STRESS moved to Shiki): the workload of the
    # R5 records (docs/vm/turn-icache.md). This image does
    # not link Shiki, and the records stay comparable.
    EMBED_TXTFILES "${R5SIM_APP_JS}"
    LDFRAGMENTS ${R5SIM_LDFRAGMENTS})
if(R5SIM_PGO STREQUAL "generate")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_PGO_GEN)
endif()
# The chunks (derby): TEXT, so a NUL follows each, as the firmware embeds them.
foreach(_chunk IN LISTS R5SIM_CHUNK_FILES)
    target_add_binary_data(${COMPONENT_LIB} "${_chunk}" TEXT)
endforeach()
if(R5SIM_APP STREQUAL "derby")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_APP_DERBY)
    # -DR5SIM_DERBY_PROF=1: cycles a turn in each pocket.derby function (and
    # the Shiki draws inside it), "R5SIM prof" lines per scene.
    if(R5SIM_DERBY_PROF)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_DERBY_PROF)
    endif()
elseif(R5SIM_APP STREQUAL "hud")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_APP_HUD)
endif()
# -DR5SIM_GUEST_ALLOC=1: the firmware guest's allocator (R3 block cache over
# heap_caps) instead of JS_NewRuntime's malloc (docs/vm/qjs-observe.md).
# Off by default so the R5/PGO records stay comparable.
if(R5SIM_GUEST_ALLOC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_GUEST_ALLOC)
    if(R5SIM_CLASSES)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "R5SIM_CLASSES=${R5SIM_CLASSES}")
    endif()
endif()
# -DR5SIM_DEVICE=1: the whole firmware session (components/pocketjs_guest)
# plus the helper-IRAM fragment (docs/vm/qjs-observe.md sec.7).
if(R5SIM_DEVICE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_DEVICE)
endif()
if(R5SIM_FRAMES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_FRAMES=${R5SIM_FRAMES})
endif()
if(R5SIM_FULL_RENDER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_FULL_RENDER)
endif()
if(R5SIM_LEVEL_UP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_LEVEL_UP)
endif()
# STRESS LV3's heap exhaustion (docs/vm/lv3-heap-exhaustion.md):
# R5SIM_GC_TRACE prints a heap/GC line per turn; R5SIM_FREE_KIB leaves only
# that much of the internal heap free after the evaluation (QEMU has no radio
# and no panel, so far more than the device); R5SIM_GC_ON_OOM collects after
# a turn whose allocations the system heap refused.
if(R5SIM_GC_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_GC_TRACE)
endif()
if(R5SIM_FREE_KIB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_FREE_KIB=${R5SIM_FREE_KIB})
endif()
if(R5SIM_GC_ON_OOM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_GC_ON_OOM)
endif()
# -DR5SIM_FIT_LIMIT_KIB=16: the firmware's pocketjs_guest_fit_limit every turn,
# with that native floor (CONFIG_POCKET_GUEST_NATIVE_FLOOR_KB).
if(R5SIM_FIT_LIMIT_KIB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_FIT_LIMIT_KIB=${R5SIM_FIT_LIMIT_KIB})
endif()
if(R5SIM_SHIKI_ROM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE R5SIM_APP_SHIKI)
    # pocket_kasane.c reaches pocket.kasane.shiki through weak references,
    # which do not pull pocket_shiki_api.c out of the archive by themselves.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u pocket_shiki_api_install" "-u pocket_shiki_api_register")
endif()
# The Shiki engine takes its C model too (no PIE, no JIT): the draw's
# instructions are not the device's, the JS turn's are.
set_source_files_properties("${R}/main/ui/kasane/ksn_render.c" "${R}/main/ui/kasane/ksn_blend_pie.c" ${R5SIM_SHIKI_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/r5sim_scalar.h")
# The firmware's per-file optimisation (main/CMakeLists.txt): these sources
# are -O2 there since docs/platform/lcd-dma-driver.md sec.9.12, and an -Os copy
# here was a different program -- -Os GCC turns every variable 64-bit shift in
# ksn_shiki.c into a libgcc call (3,000 a Shiki STRESS turn), -O2 inlines them
# (docs/vm/qjs-observe.md sec.12). Appended, so the scalar -include above stays.
set_property(SOURCE "${R}/main/text/ksn_font.c" "${R}/main/ui/kasane/ksn_render.c"
    "${R}/main/ui/kasane/ksn_core.c" "${R}/main/pocket/pocket_kasane.c"
    APPEND PROPERTY COMPILE_OPTIONS "-O2;-ffp-contract=off")
if(R5SIM_SHIKI_ROM)
    set_property(SOURCE "${R}/main/pocket/pocket_shiki_frame.c" "${R}/main/ui/kasane/ksn_shiki.c"
        APPEND PROPERTY COMPILE_OPTIONS "-O2;-ffp-contract=off")
    set_property(SOURCE "${R}/main/ui/kasane/ksn_shiki_frame.c" APPEND PROPERTY COMPILE_OPTIONS "-O2")
endif()
