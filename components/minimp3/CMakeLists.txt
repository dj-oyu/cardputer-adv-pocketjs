set(MINIMP3_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/minimp3")
if(NOT EXISTS "${MINIMP3_ROOT}/minimp3.h")
    message(FATAL_ERROR
        "minimp3 Git submodule is missing. Run git submodule update --init --checkout "
        "-- third_party/minimp3 (or python tools/prepare_minimp3.py) before building. "
        "CMake never downloads dependencies.")
endif()
idf_component_register(SRCS "minimp3.c" INCLUDE_DIRS "${MINIMP3_ROOT}")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fstack-usage)
