# libopus, decoder only, fixed point. FETCHED, NOT VENDORED.
#
# The sources live in .cache/codecs/opus-<tag>, which tools/prepare_dependencies.py
# clones at a pinned release tag the same way it pins the PocketJS upstream. Nothing
# from xiph/opus is committed to this tree; what is committed is this file, the list
# it excludes, and licenses/libopus.txt.
#
# WHICH TAG, AND WHY IT IS NOT 1.5.2. The only decode-cost measurement this project
# has (3,208 us mean per 20 ms frame, 2026-09-07, docs/opus-feasibility.md §2) was
# taken on master a6128f4 of 2026-09-06. v1.6.1 (2026-01-13) is the nearest release
# behind that commit; v1.5.2 is 2024-04-10, two and a half years of CELT changes
# further away. Pinning to the release closest to the tree that was measured is what
# keeps that number describing this build. It is still not the same tree -- the
# decode task logs its own mean and worst per stream (OPUSDEC in pocket/opus_feed.c)
# so the shipped code reports its own cost rather than inheriting one.
set(OPUS_TAG "v1.6.1")
set(OPUS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.cache/codecs/opus-1.6.1")

if(NOT EXISTS "${OPUS_DIR}/include/opus.h")
    message(FATAL_ERROR
        "libopus sources are missing from ${OPUS_DIR}.\n"
        "Run:  python tools/prepare_dependencies.py\n"
        "It clones xiph/opus at ${OPUS_TAG} into .cache/. Nothing is vendored.")
endif()

file(GLOB CELT_SRC "${OPUS_DIR}/celt/*.c")
file(GLOB SILK_SRC "${OPUS_DIR}/silk/*.c")

# DECODER ONLY.
#
# silk/fixed/ is dropped whole: every file in it is encoder analysis (autocorr,
# burg, noise shaping, pitch search). The fixed-point DECODER lives in silk/*.c
# next to the float one, selected by FIXED_POINT at compile time, not by directory.
#
# The rest is by exclusion rather than by enumeration on purpose: a new upstream
# file arrives INCLUDED, which is the failing-safe direction. A file that should not
# be here costs flash --gc-sections mostly reclaims; a file that should be here and
# is not breaks the link, loudly, at build time. The arch subdirectories go too --
# they are x86/ARM/MIPS intrinsics with no Xtensa among them.
list(FILTER CELT_SRC EXCLUDE REGEX
    "opus_custom_demo|dump_modes|celt_encoder")
string(CONCAT SILK_ENCODER_ONLY
    "enc_API|encode_indices|encode_pulses|NSQ|VAD|NLSF_encode|NLSF_VQ\.c|A2NLSF"
    "|NLSF_del_dec_quant|process_NLSFs|quant_LTP_gains|VQ_WMat_EC|HP_variable_cutoff"
    "|control_SNR|init_encoder|control_codec|check_control_input|LP_variable_cutoff"
    "|stereo_LR_to_MS|biquad_alt|ana_filt_bank_1|inner_prod_aligned|silk/debug")
list(FILTER SILK_SRC EXCLUDE REGEX "${SILK_ENCODER_ONLY}")

idf_component_register(
    SRCS ${CELT_SRC} ${SILK_SRC}
         "${OPUS_DIR}/src/opus.c"
         "${OPUS_DIR}/src/opus_decoder.c"
         "${OPUS_DIR}/src/extensions.c"
    INCLUDE_DIRS "${OPUS_DIR}/include"
    PRIV_INCLUDE_DIRS "${OPUS_DIR}" "${OPUS_DIR}/celt" "${OPUS_DIR}/silk"
                      "${OPUS_DIR}/silk/fixed")

# FIXED_POINT: no FPU reliance, and the configuration every number in
# docs/opus-feasibility.md was taken under.
# VAR_ARRAYS=1: CELT's scratch goes on the CALLER'S stack rather than into a static
# pseudo-stack. That is why libopus costs 0 bytes of static DIRAM here (measured),
# and it is why the decode task's stack has to be sized for the codec rather than
# for our own frame -- see opus_feed.c.
# DISABLE_FLOAT_API: opus_decode_float and everything it drags in. This host has
# one int16 sink.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD FIXED_POINT VAR_ARRAYS=1 DISABLE_FLOAT_API
    OPUS_VERSION="${OPUS_TAG}")

# -Os, and this is now an answer rather than a default.
#
# The project builds -Os (CONFIG_COMPILER_OPTIMIZATION_SIZE), and this component is
# the one place worth re-asking, because it is the only code in the image with a
# realtime deadline. R1 (docs/opus-feasibility.md §6) asked it on the board, same
# tree, two build directories, one variable:
#
#   -Os   mean 3,208 us   worst 3,670   stack 10,420
#   -O2   mean 3,089 us   worst 3,491   stack 10,484
#
# -3.7% on the mean for +11,920 bytes of flash. The prediction written before the
# run was -10% to -25%; it was refuted, and the rule set with it was "under 5% and
# -O2 bought nothing here". So -Os, and the flash stays.
#
# Two things that run established beyond the flag. The -Os arm reproduced the
# 2026-09-07 figure TO THE DIGIT on a tree that has moved since, which is what
# makes the 3.2 ms this whole design is sized from a measurement rather than a
# memory. And it did not reach the 2.5 ms that would have closed the PIE question
# permanently -- so that question is open and unfavourable, which is not the same
# as closed, and §4's argument against writing kernels still rests on the P4
# results and the simulator gap rather than on this.
set(OPUS_OPT "-Os" CACHE STRING "optimization level for libopus (-Os or -O2)")
# -w: upstream warnings are upstream's; -Wno-error keeps them from stopping us.
target_compile_options(${COMPONENT_LIB} PRIVATE ${OPUS_OPT} -Wno-error -w)
