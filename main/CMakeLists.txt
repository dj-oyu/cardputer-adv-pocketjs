file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(SRCS "main.c" "board.c" "shell.c" "app_session.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/generated"
    REQUIRES pocketjs_guest pocketjs_ui_core pocketjs_ui_qjs pocketjs_render_rgb565
             esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_timer esp_partition
    EMBED_TXTFILES "../apps/hello/main.js")
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/make_font.py" "${CMAKE_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_font.py")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
