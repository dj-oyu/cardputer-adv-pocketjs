# R5's offline instruction-cache model (docs/vm/r5-icache.md): STRESS's JS
# turns on an ESP32-S3 image that QEMU can run -- UART console, no display,
# no keyboard -- built with the firmware's compiler settings, so every function
# the turn runs is byte-for-byte the firmware's and a trace of it can be laid
# onto the firmware's own addresses (tools/r5sim/icache.py).
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/quickjs-ng")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(r5sim)
