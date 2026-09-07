file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(SRCS "main.c" "hal/board.c" "ui/shell.c" "scene/solar_sail.c" "scene/solar_time.c" "app_session.c" "hal/motion.c" "hal/sound.c"
    "hal/keymap.c" "text/jpfont.c" "text/skk_session.c" "ui/editor.c" "ui/codeedit.c" "ui/vimcmd.c" "text/srcstore.c" "ui/jsconsole.c" "text/jslex.c" "text/jsfont.c" "pocket/pocket_api.c" "pocket/pocket_bridge.c" "pocket/pocket_storage.c" "pocket/pocket_fs.c" "pocket/pocket_imu.c" "pocket/pocket_av.c" "pocket/pocket_io.c" "pocket/pocket_ui.c" "pocket/pocket_app.c" "pocket/pocket_net.c" "pocket/wifi_time.c" "pocket/wifi_ui.c" "ui/lessons.c" "ui/tutorial.c" "ui/paint.c" "scene/render_accel.c"
    "pet/pet_assets.c" "pet/pet_pixels.c" "pet/pet_hub.c" "pet/pet_hub_core.c" "../.cache/bmi270/bmi2.c" "../.cache/bmi270/bmi270.c"
    INCLUDE_DIRS "." "hal" "pocket" "pet" "ui" "text" "scene" "${CMAKE_BINARY_DIR}/generated" "../.cache/bmi270"
    REQUIRES pocketjs_guest pocketjs_ui_core pocketjs_ui_qjs pocketjs_render_rgb565
             skk_core ime_core
             esp_app_format esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_uart esp_driver_rmt esp_driver_usb_serial_jtag esp_timer esp_partition esp_driver_i2s esp_adc nvs_flash
             esp_wifi esp_netif esp_event lwip esp_http_client esp-tls
    EMBED_TXTFILES "../apps/hello/main.js" "../apps/imucal/imucal.js" "../apps/pet/pet.js" "../apps/companion/companion.js"
    EMBED_FILES "../apps/pet/assets/pets-compact.bin")
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_font.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_font.py")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
# The small procedural renderer benefits from speed optimization; the JS engine
# and other components retain the project's size optimization.
set_source_files_properties(shell.c solar_sail.c render_accel.c PROPERTIES COMPILE_OPTIONS "-O2")
