cmake_minimum_required(VERSION 3.22)
# Standalone registration benchmark firmware, not the Cardputer GUI image.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../../components/quickjs-ng")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(cardputer_prepared_bench)
