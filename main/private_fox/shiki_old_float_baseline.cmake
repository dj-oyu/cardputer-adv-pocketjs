# Explicit old-float comparison only. Never include assets in public distribution.
option(FOX_SHIKI_OLD_FLOAT_BASELINE "Old float fox through Shiki; NOT fixed/Q29 profile" OFF)
if(FOX_SHIKI_OLD_FLOAT_BASELINE)
  if(FOX_SHIKI_PROVIDER OR FOX_LOCAL_Q29 OR FOX_STATIC_WEIGHT_BIN OR
     KASANE_MESH_PREPARED_PROTOTYPE OR KASANE_MESH_Q7_CERTIFIED_PROTOTYPE)
    message(FATAL_ERROR "Old float baseline conflicts with fixed or prepared profiles")
  endif()
  get_target_property(_fox_existing_sources ${COMPONENT_LIB} SOURCES)
  foreach(_source IN LISTS _fox_existing_sources)
    if(_source MATCHES "(^|/)(board_fox|lease_spi|composed|hardware_probe)\\.c$")
      message(FATAL_ERROR "Old float Shiki baseline excludes legacy transport ${_source}")
    endif()
  endforeach()
  set(_fox_old_files shiki_provider.c shiki_owner.c bridge.c render_adapter.c lease.c
      eye_runtime.c scene_alpha.c composite.c prepared_alpha.c)
  foreach(_file IN LISTS _fox_old_files)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_file}")
      message(FATAL_ERROR "Old float baseline missing ${_file}")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${_file}")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/${_file}" PROPERTIES
      COMPILE_OPTIONS "-O2;-ffp-contract=off;-fno-fast-math;-fstack-usage")
  endforeach()
  target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FOX_SHIKI_OLD_FLOAT_BASELINE=1 POCKET_FOX_NATIVE_PROVIDER=1)
  message(STATUS "FOX PROFILE: OLD_FLOAT_BASELINE (not Q29/static-weight)")
endif()
