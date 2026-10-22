get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(BENCH "${REPO}/tools/vmtest/prepare/bench")
find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(COMMAND "${PYTHON}" "${BENCH}/run.py" --generate-only "${CMAKE_CURRENT_BINARY_DIR}/generated"
    RESULT_VARIABLE generated COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git -C "${REPO}" rev-parse HEAD OUTPUT_VARIABLE source_head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(KASANE "${REPO}/main/ui/kasane")
idf_component_register(SRCS "${BENCH}/bench.c" "${CMAKE_CURRENT_BINARY_DIR}/generated/runtime-rom.c"
    "${KASANE}/ksn_procedural.c" "${KASANE}/ksn_proc_analysis.c" "${KASANE}/ksn_proc_plan.c"
    "${KASANE}/ksn_proc_plan_points.c" "${KASANE}/ksn_proc_points.c"
    "${KASANE}/ksn_proc_points_dispatch.c" "${KASANE}/ksn_proc_points_pie.c"
    INCLUDE_DIRS "${REPO}/main" "${REPO}/main/pocket" "${KASANE}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    REQUIRES quickjs-ng esp_timer heap)
target_compile_definitions(${COMPONENT_LIB} PRIVATE KSN_PROC_HOST_TEST POCKET_PREPARED_TEST BENCH_HEAD="${source_head}")
# Same optimizer for the engine and adapter; this diagnostic is not the -Os GUI image.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-parameter)
