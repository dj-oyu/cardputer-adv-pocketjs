cmake_minimum_required(VERSION 3.22)
# Standalone registration benchmark firmware, not the Cardputer GUI image.
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
# Inherit product defaults, including security; only diagnostics override below.
file(WRITE "${CMAKE_BINARY_DIR}/bench-partition.defaults"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${REPO}/partitions.csv\"\n")
set(SDKCONFIG_DEFAULTS "${REPO}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_BINARY_DIR}/bench-partition.defaults")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../../components/quickjs-ng")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_prepared_bench)
