# WHEN YOU ADD A .c HERE, ADD IT BEFORE THE FIRST CALLER REFERENCES IT.
# Several agents and the owner build against this tree continuously and there is
# no signal that it has gone red except somebody else's build failing. On
# 2026-09-09 opus_net.c existed on disk with every symbol defined, and the SRCS
# row below arrived several minutes after the first call to opus_net_stop() went
# into pocket_av.c. So every object compiled and only the ELF failed -- the most
# confusing possible version of the mistake, because nothing looks wrong until
# the last step, and it blocked an agent whose own work was fine.
#
# The rule: the row lands WITH the new file, before any caller. If a change
# genuinely cannot be split that way, say up front that the tree is going red
# and for roughly how long.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(SRCS "pocket/mp3_decode.c" "pocket/mp3_feed.c" "main.c" "hal/board.c" "ui/shell.c" "scene/solar_sail.c" "scene/solar_time.c" "scene/scene_mem.c" "scene/stars.c" "app_session.c" "hal/motion.c" "hal/sound.c"
    "hal/keymap.c" "text/jpfont.c" "text/skk_session.c" "ui/editor.c" "ui/codeedit.c" "ui/vimcmd.c" "text/srcstore.c" "ui/jsconsole.c" "text/jslex.c" "text/jsfont.c" "text/textfield.c" "pocket/pocket_api.c" "pocket/pocket_bridge.c" "pocket/pocket_storage.c" "pocket/pocket_fs.c" "pocket/pocket_imu.c" "pocket/pocket_av.c" "pocket/opus_feed.c" "pocket/opus_net.c" "pocket/pocket_capture.c" "pocket/pocket_io.c" "pocket/pocket_ui.c" "pocket/pocket_text.c" "pocket/pocket_app.c" "pocket/pocket_workspace.c" "pocket/pocket_overlay.c" "ui/overlay.c" "ui/overlay_core.c" "pocket/app_registry.c" "pocket/sd_path.c" "pocket/sd_media.c" "pocket/sd_picker.c" "pocket/pocket_net.c" "pocket/pocket_tls_txfix.c" "pocket/pocket_ble.c" "pocket/wifi_time.c" "pocket/wifi_ui.c" "ui/lessons.c" "ui/tutorial.c" "ui/paint.c" "scene/render_accel.c"
    "pet/pet_assets.c" "pet/pet_pixels.c" "pet/pet_hub.c" "pet/pet_hub_core.c" "../.cache/bmi270/bmi2.c" "../.cache/bmi270/bmi270.c"
    INCLUDE_DIRS "." "hal" "pocket" "pet" "ui" "text" "scene" "${CMAKE_BINARY_DIR}/generated" "../.cache/bmi270"
    REQUIRES pocketjs_guest pocketjs_ui_core pocketjs_ui_qjs pocketjs_render_rgb565
             skk_core ime_core
             esp_app_format esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_uart esp_driver_rmt esp_driver_usb_serial_jtag esp_timer esp_partition esp_driver_i2s esp_adc nvs_flash
             esp_wifi esp_netif esp_event lwip esp_http_client esp-tls
             fatfs esp_driver_sdspi sdmmc
             opus minimp3
    EMBED_TXTFILES "../apps/hello/main.js" "../apps/imucal/imucal.js" "../apps/pet/pet.js" "../apps/companion/companion.js" "../apps/deskclock/deskclock.js" "../apps/streamplay/streamplay.js" "../apps/opusplay/opusplay.js" "../apps/opusfit/opusfit.js"
    EMBED_FILES "../apps/pet/assets/pets-compact.bin" "../apps/chime/chime.wav" "../apps/chime/chime.pok" "../apps/opusplay/tone.pok")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../apps/mp3play/mp3play.js" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../apps/mp3play/test-tone.mp3" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../apps/mp3play/test-48k.mp3" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../apps/mp3play/test-24k.mp3" BINARY)
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_font.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_font.py")
# The click PCM and the tone sine table used to be built into .bss at boot; they
# depend on nothing but compile-time constants, so they are baked here and land
# in .rodata instead. tools/test_sfx.py diffs the output against the synthesis
# that produced it (main/hal/sfx_synth.h).
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_sfx.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_sfx.py")
# The same argument for the home screen's three constant scene tables: sine,
# softness and wave_lut were 1,484 B of .bss built once at the first frame from
# nothing but literals. tools/test_scene_tables.py diffs the baked values
# against the C expressions that used to build them.
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_scene_tables.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_scene_tables.py")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
# The small procedural renderer benefits from speed optimization; the JS engine
# and other components retain the project's size optimization.
target_sources(${COMPONENT_LIB} PRIVATE "scene/flower.c" "scene/flower_species.c" "scene/glass_rain.c" "scene/wave.c" "scene/ocean.c" "scene/garden.c")
set_source_files_properties(ui/shell.c scene/solar_sail.c scene/render_accel.c scene/flower.c scene/flower_species.c scene/glass_rain.c scene/wave.c scene/ocean.c scene/garden.c PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_mbedtls_add_tx_buffer" "-Wl,-u,__wrap_esp_mbedtls_add_tx_buffer")
