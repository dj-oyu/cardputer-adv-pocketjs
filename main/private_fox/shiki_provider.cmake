# Local/private integration only: never include this asset-bearing tree in a public push.
option(FOX_SHIKI_PROVIDER "Private fixed fox through Shiki presentation (experimental)" OFF)
if(FOX_SHIKI_PROVIDER)
  if(KASANE_MESH_PREPARED_PROTOTYPE OR KASANE_MESH_Q7_CERTIFIED_PROTOTYPE)
    message(FATAL_ERROR "Private fox and public experimental prepared-mesh symbols are not yet namespaced")
  endif()
  if(NOT FOX_LOCAL_Q29)
    message(FATAL_ERROR "This verified integration profile requires -DFOX_LOCAL_Q29=ON")
  endif()
  include("${CMAKE_CURRENT_LIST_DIR}/local_asset/local_asset.cmake")
  target_sources(${COMPONENT_LIB} PRIVATE
    private_fox/shiki_provider.c private_fox/shiki_owner.c private_fox/bridge.c
    private_fox/render_adapter.c private_fox/lease.c
    private_fox/eye_runtime.c private_fox/scene_alpha.c
    private_fox/composite.c private_fox/prepared_alpha.c)
  target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FOX_SHIKI_PROVIDER=1 POCKET_FOX_NATIVE_PROVIDER=1
    FOX_RANK_PAIRS FOX_AABB_EXTREMA FOX_FRAME_WINDING FOX_TRIANGLE_VALIDATION_ONCE
    FOX_PREPARE_SNAP_ONCE FOX_COLOR_MEMO FOX_STATIC_WEIGHT_BIN FOX_DIRECT_BATCH
    FOX_WEIGHT_RECIP FOX_DEPTH_QUANT_HOIST FOX_SKIP_DQ_ORACLE FOX_FIXED_DQ_REFERENCE
    FOX_DQ_Q_FRAC=24 FOX_DQ_POS_FRAC=20 FOX_ROW_PROBE FOX_FIXED_SHADER_WEIGHTS FOX_INTERP_FRAC=26)
  set_source_files_properties(private_fox/shiki_provider.c private_fox/render_adapter.c
    private_fox/lease.c private_fox/eye_runtime.c private_fox/scene_alpha.c
    private_fox/composite.c private_fox/prepared_alpha.c PROPERTIES
    COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-fast-math;-fstack-usage")
  # No bridge/composed/board_fox or transport mocks: Kasane alone presents.
endif()
