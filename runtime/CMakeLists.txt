cmake_minimum_required(VERSION 3.25)
project(MH3Recompiled LANGUAGES C CXX ASM)
option(MH3_ENABLE_GUEST_CALL_TRACE "Enable temporary guest call graph instrumentation" OFF)
option(MH3_ENABLE_COLLISION_CHECK "Build isolated Dolphin collision counterexample checks" OFF)
option(MH3_WRITE_WATCH "Debug: report the guest code storing to MH3_WRITE_WATCH=start,length (rebuilds all generated code)" OFF)
option(MH3_ENABLE_EFB_COPY_TRACE "Debug: page-protect GX texture-copy destinations and log host accesses" OFF)
# Development-only runtime features, compiled out of release builds: MH3_STATS
# speed line, MH3_PROFILE_FRAMES profile and GX counters, MH3_CAPTURE_* frame
# dumps, MH3_FRAMESKIP, and scripted input (MH3_INPUT_FILE / _RECORD / _PLAYBACK).
option(MH3_DEVELOPMENT "Build development/debug features (stats, captures, scripted input)" OFF)
option(MH3_ENABLE_GX_DIAGNOSTICS "Enable controlled renderer isolation and state logging" OFF)
# Supported targets: arm32/arm64 Linux with GLES, and Vita with native GXM.
# MH3_ALLOW_UNSUPPORTED_HOST keeps other hosts
# configurable for development only; they are not a supported product.
option(MH3_ALLOW_UNSUPPORTED_HOST "Configure on a host other than arm32/arm64 Linux" OFF)
if(NOT VITA AND (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|armv7l|armv7|armv8l|arm)$"))
  if(NOT MH3_ALLOW_UNSUPPORTED_HOST)
    message(FATAL_ERROR "MH3 targets arm32/arm64 Linux; this is ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
      "Cross-compile with cmake/toolchains/linux-arm32.cmake or linux-arm64.cmake, or set MH3_ALLOW_UNSUPPORTED_HOST=ON.")
  endif()
  message(WARNING "Configuring for unsupported host ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(MH3_AURORA_ROOT)
  enable_language(C)
  # The game thread owns the native renderer; GPU submissions are asynchronous.
  add_subdirectory("${MH3_AURORA_ROOT}" "${CMAKE_BINARY_DIR}/aurora")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The translated ISA helpers require Clang")
endif()
include(../generated/build_shards/shards.cmake)
if(VITA)
  set(MH3_GUEST_MEMORY_SOURCE vita/guest_memory.cpp)
else()
  set(MH3_GUEST_MEMORY_SOURCE src/guest_flat_memory.cpp)
endif()
add_executable(MH3
  src/main.cpp src/memory.cpp ${MH3_GUEST_MEMORY_SOURCE} src/os_boot.cpp src/os_sleep.cpp src/os_alarm.cpp src/os_devices.cpp src/abi_bridge.cpp
  src/ppc_helpers.cpp src/fpu_helpers.cpp src/executable_guard.cpp src/rso_services.cpp src/wpad_services.cpp src/controller_host.cpp src/controller_tests.cpp src/village_boot.cpp src/audio_interface.cpp
  src/audio/ax_mix.cpp src/audio/ax_memory.cpp src/audio/ax_sync.cpp src/audio_services.cpp src/audio_tests.cpp src/audio_dma.cpp
  src/device_tests.cpp src/efb_host.cpp src/ios.cpp src/usb_hid_services.cpp src/disc_image.cpp src/video_interface.cpp src/gx_registers.cpp src/gx_stream.cpp src/gx_services.cpp src/vi_services.cpp src/glare_filter.cpp src/frame_pacer.cpp src/fps_timers.cpp src/game_frame.cpp src/mhchar_motion.cpp src/game_step.cpp src/effect_step.cpp src/particle_manager_calc.cpp src/map_sound.cpp src/player_timers.cpp src/port_check.cpp src/pulse.cpp src/rumble_timers.cpp src/step_counter.cpp src/swing_effect.cpp src/stamina.cpp src/player_turn.cpp src/field_jump.cpp src/save_data.cpp src/game_tasks.cpp src/player_health.cpp src/tick_sites.cpp src/camera_script.cpp src/movie_draw.cpp src/reset_fade.cpp src/loading_screen.cpp src/quest_clock.cpp src/ambient_hover.cpp src/group_shuttle.cpp src/hit_timers.cpp src/enemy_glide.cpp src/creature_fade.cpp src/ambient_flutter.cpp src/yure_chain.cpp src/audio_reverb.cpp src/anm_chr_keys.cpp src/anm_chr_blend.cpp src/effect_texture.cpp src/effect_billboard.cpp src/msl_math.cpp src/sound_listener.cpp src/texture_scroll.cpp src/screen_fade.cpp src/enemy_timers.cpp src/shell_motion.cpp src/enemy_turn.cpp src/object_ages.cpp src/village_player_actions.cpp src/field_player_actions.cpp src/g3d_res.cpp src/g3d_calc_world.cpp src/mtx.cpp src/ef_anim_curve.cpp src/nw4r_accessors.cpp
  ../generated/data_sections_init.cpp ../generated/data_sections_init_blobs.S
  ../generated/rso_overlays/map_village.cpp ../generated/rso_overlays/quest_field.cpp ../generated/rso_overlays/enemy_field.cpp ../generated/rso_overlays/map01_field.cpp ../generated/rso_overlays/map00_field.cpp ../generated/rso_overlays/map02_field.cpp ../generated/rso_overlays/map03_field.cpp ../generated/rso_overlays/map04_field.cpp ../generated/rso_overlays/map05_field.cpp ../generated/rso_overlays/map06_field.cpp ../generated/rso_overlays/map07_field.cpp ../generated/rso_overlays/map08_field.cpp ../generated/rso_overlays/map09_field.cpp ../generated/rso_overlays/map10_field.cpp ../generated/rso_overlays/map11_field.cpp
  ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
  ${MKW_BASE_REGISTRATION_SOURCES})
target_sources(MH3 PRIVATE src/gx_panic.cpp src/gx_panic_tests.cpp src/model_resources.cpp src/model_submission_trace.cpp
  native/mh3_model_resources.c native/mh3_model_runtime.c native/mh3_model_draw.c)
target_sources(MH3 PRIVATE native/mh3_model_matrices.c)
target_sources(MH3 PRIVATE src/model_catalog.cpp native/mh3_model_catalog.c
  native/mh3_model_submission.c)
target_sources(MH3 PRIVATE src/model_vertex_sources.cpp native/mh3_vertex_sources.c)
target_sources(MH3 PRIVATE src/lighting.cpp native/mh3_lighting.c)
target_sources(MH3 PRIVATE src/native_frame.cpp)
target_sources(MH3 PRIVATE src/render_mode.cpp native/mh3_render_mode.c)
target_sources(MH3 PRIVATE src/native_display_utilities.cpp)
target_sources(MH3 PRIVATE native/mh3_draw_state.c)
target_sources(MH3 PRIVATE src/model_state.cpp src/model_scene_state.cpp native/mh3_model_state.c
  src/model_texture_matrices.cpp native/mh3_texture_matrices.c native/mh3_material_state.c
  src/native_ui.cpp native/mh3_ui.c
  src/native_effect_draw.cpp native/mh3_effect_draw.c
  src/native_scene_feedback.cpp native/mh3_scene_feedback.c
  native/mh3_demo_menu.c
  src/native_scene_queue.cpp native/mh3_scene_queue.c
  src/launcher_support.cpp)
# Clang's precise float pragma is unsupported for the Vita target. Keep the
# CPU fog/light producer's range/finite checks and operation order intact.
set_source_files_properties(native/mh3_material_state.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
set_source_files_properties(native/mh3_scene_queue.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
target_include_directories(MH3 PRIVATE include ..)
# glibc's backtrace_symbols only names functions in the dynamic symbol table,
# so export ours for symbolized MH3_TRACE_ERRORS stack traces.
if(NOT VITA)
  set_target_properties(MH3 PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(MH3 PRIVATE ${CMAKE_DL_LIBS})
endif()
target_compile_options(MH3 PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-O2;-gline-tables-only;-ffp-contract=off>)
if(MH3_ENABLE_GUEST_CALL_TRACE)
  target_sources(MH3 PRIVATE src/guest_call_trace.cpp)
  set(trace_manifest "${CMAKE_SOURCE_DIR}/../generated/build_shards/guest_trace_sources.cmake")
  if(NOT EXISTS "${trace_manifest}")
    message(FATAL_ERROR "Run scripts/trace_guest_calls.py enable before enabling guest call tracing")
  endif()
  include("${trace_manifest}")
  foreach(trace_source IN LISTS MH3_GUEST_TRACE_SOURCES)
    set_source_files_properties("${trace_source}" PROPERTIES
      COMPILE_DEFINITIONS MH3_ENABLE_GUEST_CALL_TRACE=1)
  endforeach()
endif()
enable_testing()
if(MH3_ENABLE_COLLISION_CHECK)
  target_sources(MH3 PRIVATE src/collision_check.cpp)
  set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS MH3_ENABLE_COLLISION_CHECK=1)
  add_test(NAME mh3_collision_check COMMAND MH3 --collision-check)
endif()
add_test(NAME mh3_runtime_self_test COMMAND MH3 --self-test)
add_test(NAME mh3_audio_self_test COMMAND MH3 --audio-self-test)
if(TARGET aurora::core)
  target_sources(MH3 PRIVATE src/graphics_host.cpp src/native_render_state.cpp src/native_texture_state.cpp
    src/native_material.cpp src/character_material.cpp)
  if(VITA)
    target_sources(MH3 PRIVATE vita/audio_output.cpp)
  else()
    target_sources(MH3 PRIVATE src/audio_output.cpp)
  endif()
  if(MH3_ENABLE_GX_DIAGNOSTICS)
    set_property(SOURCE src/graphics_host.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_ENABLE_GX_DIAGNOSTICS=1)
    target_link_libraries(MH3 PRIVATE absl::flat_hash_map)
  endif()
  if(MH3_ENABLE_EFB_COPY_TRACE)
    target_sources(MH3 PRIVATE src/efb_copy_trace.cpp)
    set_property(SOURCE src/graphics_host.cpp src/gx_stream.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_ENABLE_EFB_COPY_TRACE=1)
  endif()
  set_source_files_properties(src/graphics_host.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_render_state.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_texture_state.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_material.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  target_compile_definitions(MH3 PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3 PRIVATE aurora::core aurora::gx aurora::vi)
  if(NOT VITA)
    add_executable(mh3_native_geometry_check EXCLUDE_FROM_ALL
      "${MH3_AURORA_ROOT}/tests/native_geometry_check.cpp"
      src/native_render_state.cpp native/mh3_draw_state.c)
    target_include_directories(mh3_native_geometry_check PRIVATE "${MH3_AURORA_ROOT}/lib" include)
    target_compile_features(mh3_native_geometry_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_geometry_check PRIVATE aurora::core aurora::gx aurora::vi)
    add_executable(mh3_native_material_check EXCLUDE_FROM_ALL
      ../tests/native/native_material_check.cpp src/native_material.cpp src/native_render_state.cpp
      native/mh3_material_state.c native/mh3_draw_state.c)
    target_include_directories(mh3_native_material_check PRIVATE "${MH3_AURORA_ROOT}/lib" include)
    target_compile_features(mh3_native_material_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_material_check PRIVATE aurora::core aurora::gx aurora::vi)
    add_executable(mh3_native_scene_feedback_check EXCLUDE_FROM_ALL
      "${MH3_AURORA_ROOT}/tests/native_scene_feedback_check.cpp")
    target_include_directories(mh3_native_scene_feedback_check PRIVATE "${MH3_AURORA_ROOT}/lib")
    target_compile_features(mh3_native_scene_feedback_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_scene_feedback_check PRIVATE aurora::core aurora::gx aurora::vi)
    add_executable(mh3_native_mesh_buffer_check EXCLUDE_FROM_ALL
      "${MH3_AURORA_ROOT}/tests/native_mesh_buffer_check.cpp")
    target_include_directories(mh3_native_mesh_buffer_check PRIVATE "${MH3_AURORA_ROOT}/lib")
    target_compile_features(mh3_native_mesh_buffer_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_mesh_buffer_check PRIVATE aurora::core aurora::gx aurora::vi)
  endif()
endif()

if(VITA)
  if(NOT TARGET aurora::core OR NOT AURORA_GXM)
    message(FATAL_ERROR "The Vita game requires MH3_AURORA_ROOT and AURORA_GXM=ON")
  endif()
  if(MH3_ENABLE_EFB_COPY_TRACE OR MH3_WRITE_WATCH)
    message(FATAL_ERROR "Page protection diagnostics require the Linux guest memory implementation")
  endif()
  include("${VITASDK}/share/vita.cmake" REQUIRED)
  target_sources(MH3 PRIVATE vita/platform.cpp vita/launcher.cpp vita/launcher_font.cpp
    src/launcher_disc.cpp src/launcher_textures.cpp)
  set(rom_decoder_dir "${CMAKE_CURRENT_SOURCE_DIR}/vita/rom_decoder")
  set(rom_decoder_script "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-vita-rom-decoder.sh")
  set(rom_decoder_archive "${CMAKE_BINARY_DIR}/rom-decoder/armv7-sony-vita-newlibeabihf/release/libmh3_rom_decoder.a")
  add_custom_command(OUTPUT "${rom_decoder_archive}"
    COMMAND "${CMAKE_COMMAND}" -E env "VITASDK=${VITASDK}"
      bash "${rom_decoder_script}" "${CMAKE_BINARY_DIR}/rom-decoder"
    DEPENDS "${rom_decoder_script}" "${rom_decoder_dir}/Cargo.toml"
      "${rom_decoder_dir}/Cargo.lock" "${rom_decoder_dir}/src/lib.rs"
      "${CMAKE_CURRENT_SOURCE_DIR}/vita/rom_decoder.h"
    COMMENT "Building Vita ROM decoder with clang 21 codecs"
    VERBATIM)
  add_custom_target(MH3RomDecoderBuild DEPENDS "${rom_decoder_archive}")
  add_library(MH3RomDecoder STATIC IMPORTED)
  set_target_properties(MH3RomDecoder PROPERTIES IMPORTED_LOCATION "${rom_decoder_archive}")
  add_dependencies(MH3RomDecoder MH3RomDecoderBuild)
  target_link_libraries(MH3 PRIVATE MH3RomDecoder)
  set_property(SOURCE src/launcher_disc.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_HAS_ROM_DECODER=1)
  set_source_files_properties(src/launcher_textures.cpp vita/launcher.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  if(MH3_DEVELOPMENT)
    set_property(SOURCE vita/platform.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
  endif()
  target_link_libraries(MH3 PRIVATE kubridge_stub SceAudio_stub ScePower_stub SceAppMgr_stub
    ScePvf_stub SceVshBridge_stub SceIme_stub SceSysmodule_stub)
  set_target_properties(MH3 PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vita/ldscript.ld")
  set(VITA_MAKE_FSELF_FLAGS "-a 0x2800000000000001")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
  vita_create_self(eboot.bin MH3 UNSAFE NOASLR)
  vita_create_vpk(mh3-vita.vpk MH3WII001 eboot.bin VERSION 00.01 NAME "Monster Hunter Tri"
    FILE "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/DSP/dsp_coef.bin" "dsp_coef.bin")
endif()

if(MH3_WRITE_WATCH)
  target_sources(MH3 PRIVATE src/write_watch.cpp)
  target_compile_definitions(MH3 PRIVATE MH3_WRITE_WATCH=1)
endif()

if(MH3_DEVELOPMENT OR VITA)
  target_sources(MH3 PRIVATE src/quest_catalog.cpp) # --list, --quest
endif()
if(MH3_DEVELOPMENT)
  target_sources(MH3 PRIVATE src/slow_path_trace.cpp) # MH3_SLOW_PATH_TRACE
endif()

# MH3_DEVELOPMENT applies to the runtime's own sources only, so toggling it
# does not rebuild the generated shards.
if(MH3_DEVELOPMENT)
  get_target_property(MH3_ALL_SOURCES MH3 SOURCES)
  list(FILTER MH3_ALL_SOURCES INCLUDE REGEX "^src/")
  set_property(SOURCE ${MH3_ALL_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
endif()

if(TARGET aurora::core)
  if(NOT VITA)
    add_executable(MH3VitaTexturePack ../scripts/vita_texture_pack.cpp)
    target_include_directories(MH3VitaTexturePack PRIVATE "${MH3_AURORA_ROOT}/lib")
    target_link_libraries(MH3VitaTexturePack PRIVATE "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx,aurora::vi>")
    add_executable(MH3LauncherTests EXCLUDE_FROM_ALL src/launcher_tests.cpp
      src/launcher_support.cpp src/launcher_disc.cpp src/launcher_textures.cpp src/disc_image.cpp)
    target_include_directories(MH3LauncherTests PRIVATE include "${MH3_AURORA_ROOT}/lib")
    target_link_libraries(MH3LauncherTests PRIVATE "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx,aurora::vi>")
  endif()
  add_executable(MH3ControllerHostTest src/controller_sdl_tests.cpp src/controller_host.cpp)
  target_include_directories(MH3ControllerHostTest PRIVATE include)
  target_compile_definitions(MH3ControllerHostTest PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3ControllerHostTest PRIVATE ${AURORA_SDL3_TARGET})
  add_test(NAME mh3_sdl_controller_test COMMAND MH3ControllerHostTest)
endif()
