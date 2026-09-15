# MEASUREMENT HARNESS COMPONENT -- worktree only, never committed, never shipped.
# Builds libopus (fixed point) for the S3 so the decode cost can be measured on
# the real part. Sources are read from .cache/codecs/opus, which is gitignored;
# nothing is vendored into the tree.
set(OPUS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.cache/codecs/opus")

file(GLOB CELT_SRC   "${OPUS_DIR}/celt/*.c")
file(GLOB SILK_SRC   "${OPUS_DIR}/silk/*.c")
file(GLOB SILKF_SRC  "${OPUS_DIR}/silk/fixed/*.c")
# opus_demo/tests/mode-dumpers carry their own main(); keep them out.
list(FILTER CELT_SRC EXCLUDE REGEX "(opus_custom_demo|dump_modes)")

idf_component_register(
    SRCS ${CELT_SRC} ${SILK_SRC} ${SILKF_SRC}
         "${OPUS_DIR}/src/opus.c"
         "${OPUS_DIR}/src/opus_decoder.c"
         "${OPUS_DIR}/src/extensions.c"
    INCLUDE_DIRS "${OPUS_DIR}/include"
    PRIV_INCLUDE_DIRS "${OPUS_DIR}" "${OPUS_DIR}/celt" "${OPUS_DIR}/silk" "${OPUS_DIR}/silk/fixed")

# FIXED_POINT: no FPU reliance, the normal embedded choice.
# VAR_ARRAYS: CELT scratch goes on the CALLER'S STACK -- which is exactly the
# 15,968-byte demand measured on the host, and what the harness must size for.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD FIXED_POINT VAR_ARRAYS=1 OPUS_VERSION="harness")
# R1 (2026-09-09): the optimization level is now a cache variable so that -Os and
# -O2 can exist as two build directories of ONE tree, which is what makes the two
# mean_us readings comparable. Nothing else in the harness changed.
#   idf.py -B build_opus_os -DOPUS_OPT=-Os build     (the 2026-09-07 configuration)
#   idf.py -B build_opus_o2 -DOPUS_OPT=-O2 build     (R1)
set(OPUS_OPT "-Os" CACHE STRING "optimization level for libopus")
target_compile_options(${COMPONENT_LIB} PRIVATE ${OPUS_OPT} -Wno-error -w)
