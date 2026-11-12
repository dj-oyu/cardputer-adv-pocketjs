# IDF scans requirements before normal cache options/generated files exist.
if(CMAKE_BUILD_EARLY_EXPANSION)
 idf_component_register(REQUIRES quickjs-ng esp_timer esp_hw_support log esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_i2s)
 return()
endif()
set(R "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(NOT SKIN_ASSET_INPUT)
 message(FATAL_ERROR "Set SKIN_ASSET_INPUT to run_generated_app.py output (fixed public receipt only)")
endif()
set(G "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${G}")
execute_process(COMMAND "${PYTHON}" "${R}/tools/make_font.py" "${G}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PYTHON}" "${R}/tools/r5sim/pet_header.py" "${R}/apps/pet/assets/pets-compact.bin" "${G}/kasane_pet_test_data.h" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PYTHON}" "${R}/tools/skinasset_sim/generate.py" "${SKIN_ASSET_INPUT}" "${G}" COMMAND_ERROR_IS_FATAL ANY)
include("${G}/app_chunks.cmake")
idf_component_register(SRCS "skinasset_sim.c" "${G}/app_chunks.c" "${G}/ksn_shiki_rom_none.c"
 "${R}/tools/hostshim/pocket_api_stub.c"
 "${R}/tools/hostshim/pocket_surface_stubs.c"
 "${R}/main/text/ksn_font.c"
 "${R}/tools/hostshim/jpfont.c"
 "${R}/main/pet/ksn_pet.c"
 "${R}/main/pet/pet_pixels.c"
 "${R}/tools/hostshim/ksn_pet_builtin.c"
 "${R}/main/pocket/pocket_clock_source.c"
 "${R}/main/pocket/pocket_clock.c"
 "${R}/main/pocket/pocket_av_playback_source.c"
 "${R}/main/pocket/pocket_av_output_source.c"
 "${R}/main/pocket/app_view_assets.c"
 "${R}/main/pocket/app_view_provider.c"
 "${R}/main/pocket/app_music_view.c"
 "${R}/main/pocket/app_legacy_presenter.c"
 "${R}/main/pocket/pocket_kasane.c"
 "${R}/main/pocket/pocket_shiki_frame.c"
 "${R}/main/ayatsuri/ayatsuri_model.c"
 "${R}/main/ayatsuri/ayatsuri_visibility.c"
 "${R}/main/ayatsuri/compact_opaque.c"
 "${R}/main/ui/kasane/ksn_mesh_depth.c"
 "${R}/main/pocket/shiki_heap.c"
 "${R}/main/ui/kasane/ksn_shiki.c"
 "${R}/main/ui/kasane/ksn_shiki_frame.c"
 "${R}/main/ui/kasane/ksn_runtime.c"
 "${R}/main/ui/kasane/ksn_schema.c"
 "${R}/main/ui/kasane/ksn_schema_session.c"
 "${R}/main/ui/kasane/ksn_source.c"
 "${R}/main/ui/kasane/ksn_source_pool.c"
 "${R}/main/ui/kasane/ksn_source_pool_adapter.c"
 "${R}/main/ui/kasane/ksn_core.c"
 "${R}/main/ui/kasane/ksn_view.c"
 "${R}/main/ui/kasane/ksn_cache.c"
 "${R}/main/ui/kasane/ksn_modal.c"
 "${R}/main/ui/kasane/ksn_render.c"
 "${R}/main/ui/kasane/ksn_blend_pie.c"
 "${R}/main/pocket/app_notice.c"
 "${R}/main/pocket/pocket_shape.c"
 "${R}/main/pocket/pocket_shiki_api.c"
 "${R}/main/pocket/pocket_app_load.c"
 INCLUDE_DIRS "." "${R}/main" "${R}/main/pocket" "${R}/main/ui" "${R}/main/ui/kasane" "${R}/main/ayatsuri" "${R}/main/text" "${R}/main/hal" "${G}"
 REQUIRES quickjs-ng esp_timer esp_hw_support log esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_i2s
 EMBED_TXTFILES ${APP_CHUNK_FILES} "${G}/skin_asset_bootstrap.js")
# QEMU does not implement the S3 PIE instructions. Target-native scalar renderer,
# never report this diagnostic as device PIE/performance acceptance.
set_source_files_properties("${R}/main/ui/kasane/ksn_render.c" "${R}/main/ui/kasane/ksn_blend_pie.c" "${R}/main/ui/kasane/ksn_shiki.c" PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/scalar.h")
