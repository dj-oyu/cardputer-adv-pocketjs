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
# -DR5SIM_APP=shiki: today's STRESS (apps/stress/stress.js, drawn with
# pocket.kasane.shiki), as tools/build_stress_app_test.sh builds it on the host:
# the app's Shiki programs become the built-in table (Node 16+). Default: the
# R5 records' Kasane-retained STRESS (stress_r5.js). docs/vm/qjs-observe.md sec.12.
set(R5SIM_APP_SOURCES "")
set(R5SIM_APP_JS "${CMAKE_CURRENT_LIST_DIR}/stress_r5.js")
set(R5SIM_SHIKI_SOURCES "")
if(R5SIM_APP STREQUAL "shiki")
    find_program(R5SIM_NODE node REQUIRED)
    execute_process(COMMAND "${R5SIM_NODE}" "${R}/tools/kasane_ir/emit_shiki_rom.mjs"
        "${CMAKE_BINARY_DIR}/generated/stress_shiki_rom.c" "stress=apps/stress/stress_prog.js"
        WORKING_DIRECTORY "${R}" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
    set(R5SIM_SHIKI_SOURCES "${R}/main/pocket/pocket_shiki_frame.c" "${R}/main/pocket/shiki_heap.c"
        "${R}/main/ui/kasane/ksn_shiki.c" "${R}/main/ui/kasane/ksn_shiki_frame.c"
        "${R}/main/pocket/pocket_shiki_api.c")
    set(R5SIM_APP_SOURCES ${R5SIM_SHIKI_SOURCES} "${CMAKE_BINARY_DIR}/generated/stress_shiki_rom.c")
    set(R5SIM_APP_JS "${R}/apps/stress/stress.js")
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
if(R5SIM_APP STREQUAL "shiki")
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
if(R5SIM_APP STREQUAL "shiki")
    set_property(SOURCE "${R}/main/pocket/pocket_shiki_frame.c" "${R}/main/ui/kasane/ksn_shiki.c"
        APPEND PROPERTY COMPILE_OPTIONS "-O2;-ffp-contract=off")
    set_property(SOURCE "${R}/main/ui/kasane/ksn_shiki_frame.c" APPEND PROPERTY COMPILE_OPTIONS "-O2")
endif()
