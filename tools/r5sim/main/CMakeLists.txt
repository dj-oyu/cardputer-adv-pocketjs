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
    INCLUDE_DIRS "." "${R}/main" "${R}/main/pocket" "${R}/main/ui" "${R}/main/ui/kasane"
        "${R}/main/text" "${R}/main/hal" "${CMAKE_BINARY_DIR}/generated"
    REQUIRES quickjs-ng esp_timer esp_hw_support log esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_i2s
    EMBED_TXTFILES "${R}/apps/stress/stress.js")
set_source_files_properties("${R}/main/ui/kasane/ksn_render.c" "${R}/main/ui/kasane/ksn_blend_pie.c"
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/r5sim_scalar.h")
