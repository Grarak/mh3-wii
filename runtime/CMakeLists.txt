cmake_minimum_required(VERSION 3.25)
project(MH3Recompiled LANGUAGES C CXX ASM)
option(MH3_ENABLE_GUEST_CALL_TRACE "Enable temporary guest call graph instrumentation" OFF)
option(MH3_ENABLE_COLLISION_CHECK "Build isolated Dolphin collision counterexample checks" OFF)
option(MH3_WRITE_WATCH "Debug: report the guest code storing to MH3_WRITE_WATCH=START,LENGTH (rebuilds all generated code)" OFF)
option(MH3_ENABLE_EFB_COPY_TRACE "Debug: page-protect GX texture-copy destinations and log host accesses" OFF)
# Development-only runtime features, compiled out of release builds: MH3_STATS
# speed line, MH3_PROFILE profile and GX counters, MH3_CAPTURE frame
# dumps, MH3_FRAMESKIP, and scripted input (MH3_INPUT_FILE / _RECORD / _PLAYBACK).
option(MH3_DEVELOPMENT "Build development/debug features (stats, captures, scripted input)" OFF)
# Quest selection and native launcher features are present in every build.
# Assertions follow CMAKE_BUILD_TYPE/NDEBUG independently of instrumentation.
option(MH3_PROFILE_FRAME_POINTERS "Emit frame pointers and unwind tables for CPU caller profiling" ${MH3_DEVELOPMENT})
if(MH3_PROFILE_FRAME_POINTERS)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer;-mno-omit-leaf-frame-pointer;-funwind-tables>")
  if(VITA OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv[78].*|arm)$")
    # Keep one documented ARM32 frame convention even in Thumb code. This is
    # AAPCS (previous FP and LR at r11), not Linux's legacy APCS perf walker.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mframe-chain=aapcs+leaf>)
  endif()
endif()
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
# Native AS wrappers retain their original translation, but become dynamic
# winners. Refresh indirect tables AND direct-call traits before loading the
# graph. This gate applies to Vita and ordinary Linux/ARM32 CMake builds.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(MH3_NATIVE_DISPATCH_RESOLVED_PROFILE "" CACHE FILEPATH "Optional resolved translator mod profile")
set(MH3_NATIVE_DISPATCH_RETRO_CPP_DIR "" CACHE PATH "Optional translator mod C++ directory")
set(native_dispatch_args)
if(MH3_NATIVE_DISPATCH_RESOLVED_PROFILE)
  list(APPEND native_dispatch_args --resolved-profile "${MH3_NATIVE_DISPATCH_RESOLVED_PROFILE}")
endif()
if(MH3_NATIVE_DISPATCH_RETRO_CPP_DIR)
  list(APPEND native_dispatch_args --retro-cpp-dir "${MH3_NATIVE_DISPATCH_RETRO_CPP_DIR}")
endif()
file(GLOB_RECURSE native_dispatch_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
file(GLOB native_dispatch_tools CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../toolchain/translator/src/Translator.Cli/bin/Release/net8.0/Translator*.dll")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${native_dispatch_sources} ${native_dispatch_tools}
  "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare_native_dispatch.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/../generated/translation.json")
file(GLOB native_dispatch_functions CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../generated/functions/*.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${native_dispatch_functions})
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../generated/translation.json" native_dispatch_metadata)
string(JSON native_dispatch_bundle_type ERROR_VARIABLE native_dispatch_bundle_error TYPE "${native_dispatch_metadata}" sourceBundlePath)
if(NOT native_dispatch_bundle_error AND native_dispatch_bundle_type STREQUAL "STRING")
  string(JSON native_dispatch_bundle GET "${native_dispatch_metadata}" sourceBundlePath)
  cmake_path(ABSOLUTE_PATH native_dispatch_bundle BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../generated")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native_dispatch_bundle}")
endif()
if(MH3_NATIVE_DISPATCH_RESOLVED_PROFILE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MH3_NATIVE_DISPATCH_RESOLVED_PROFILE}")
endif()
if(MH3_NATIVE_DISPATCH_RETRO_CPP_DIR)
  file(GLOB_RECURSE native_dispatch_mod_sources CONFIGURE_DEPENDS "${MH3_NATIVE_DISPATCH_RETRO_CPP_DIR}/*")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${native_dispatch_mod_sources})
  cmake_path(GET MH3_NATIVE_DISPATCH_RETRO_CPP_DIR PARENT_PATH native_dispatch_mod_parent)
  if(EXISTS "${native_dispatch_mod_parent}/translated_sources.bin")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native_dispatch_mod_parent}/translated_sources.bin")
  endif()
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare_native_dispatch.py"
  ${native_dispatch_args} RESULT_VARIABLE native_dispatch_result)
if(NOT native_dispatch_result EQUAL 0)
  message(FATAL_ERROR "Native dispatch preparation failed; generated profile graph was not accepted")
endif()
include(../generated/build_shards/shards.cmake)
if(VITA)
  set(MH3_GUEST_MEMORY_SOURCE vita/guest_memory.cpp)
else()
  set(MH3_GUEST_MEMORY_SOURCE src/guest_flat_memory.cpp)
endif()
add_executable(MH3
  src/main.cpp src/memory.cpp ${MH3_GUEST_MEMORY_SOURCE} src/os_boot.cpp src/os_sleep.cpp src/os_alarm.cpp src/os_devices.cpp src/abi_bridge.cpp
  src/ppc_helpers.cpp src/fpu_helpers.cpp src/executable_guard.cpp src/rso_services.cpp src/wpad_services.cpp src/controller_host.cpp native/mh3_classic_map.c src/controller_tests.cpp src/village_boot.cpp src/audio_interface.cpp
  src/audio_tests.cpp src/audio_dma.cpp
  src/audio/native_audio_api.cpp src/audio/native_audio_frame.cpp src/audio/native_audio_mixer.cpp src/audio/native_audio_tests.cpp src/audio/sdk_mix.cpp src/audio/sdk_synseq.cpp src/audio/sound_engine.cpp src/audio/sound_stream.cpp
  src/device_tests.cpp src/efb_host.cpp src/ios.cpp src/usb_hid_services.cpp src/disc_image.cpp src/video_interface.cpp src/gx_registers.cpp src/gx_stream.cpp src/gx_services.cpp src/vi_services.cpp src/glare_filter.cpp src/frame_pacer.cpp src/fps_timers.cpp src/game_frame.cpp src/mhchar_motion.cpp src/game_step.cpp src/effect_step.cpp src/particle_manager_calc.cpp src/map_sound.cpp src/player_timers.cpp src/port_check.cpp src/pulse.cpp src/rumble_timers.cpp src/pad_input.cpp src/camera_input.cpp src/field_camera.cpp src/player_aim.cpp src/step_counter.cpp src/swing_effect.cpp src/stamina.cpp src/player_turn.cpp src/field_jump.cpp src/save_data.cpp src/game_tasks.cpp src/player_health.cpp src/tick_sites.cpp src/camera_script.cpp src/movie_draw.cpp src/reset_fade.cpp src/loading_screen.cpp src/quest_clock.cpp src/ambient_hover.cpp src/group_shuttle.cpp src/hit_timers.cpp src/enemy_glide.cpp src/creature_fade.cpp src/ambient_flutter.cpp src/yure_chain.cpp src/audio_reverb.cpp src/anm_chr_keys.cpp src/anm_chr_blend.cpp src/anm_obj_chr_res.cpp src/effect_texture.cpp src/effect_billboard.cpp src/msl_math.cpp src/texture_scroll.cpp src/screen_fade.cpp src/enemy_timers.cpp src/shell_motion.cpp src/enemy_turn.cpp src/object_ages.cpp src/village_player_actions.cpp src/field_player_actions.cpp src/g3d_res.cpp src/g3d_calc_world.cpp src/mtx.cpp src/ef_anim_curve.cpp src/nw4r_accessors.cpp
  ../generated/data_sections_init.cpp ../generated/data_sections_init_blobs.S
  ../generated/rso_overlays/map_village.cpp ../generated/rso_overlays/quest_field.cpp ../generated/rso_overlays/enemy_field.cpp ../generated/rso_overlays/map01_field.cpp ../generated/rso_overlays/map00_field.cpp ../generated/rso_overlays/map02_field.cpp ../generated/rso_overlays/map03_field.cpp ../generated/rso_overlays/map04_field.cpp ../generated/rso_overlays/map05_field.cpp ../generated/rso_overlays/map06_field.cpp ../generated/rso_overlays/map07_field.cpp ../generated/rso_overlays/map08_field.cpp ../generated/rso_overlays/map09_field.cpp ../generated/rso_overlays/map10_field.cpp ../generated/rso_overlays/map11_field.cpp
  ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
  ${MKW_BASE_REGISTRATION_SOURCES})
target_sources(MH3 PRIVATE src/gx_panic.cpp src/gx_panic_tests.cpp src/model_resources.cpp src/model_submission_trace.cpp
  native/mh3_model_resources.c native/mh3_model_runtime.c native/mh3_model_draw.c)
target_sources(MH3 PRIVATE src/model_draw_dispatch.cpp native/mh3_model_draw_dispatch.c)
set_source_files_properties(native/mh3_model_draw_dispatch.c
  PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
target_sources(MH3 PRIVATE native/mh3_model_matrices.c)
target_sources(MH3 PRIVATE src/model_catalog.cpp native/mh3_model_catalog.c
  native/mh3_model_submission.c native/mh3_mesh_pack.c native/mh3_material_pack.c)
target_sources(MH3 PRIVATE src/model_material.cpp native/mh3_model_material_updates.c
  native/mh3_model_gpu_inputs.c src/model_material_prep.cpp native/mh3_model_material_prep.c)
target_sources(MH3 PRIVATE src/model_scope.cpp src/model_render_prep.cpp native/mh3_model_scope.c
  native/mh3_model_prepare_cache.c)
set_source_files_properties(native/mh3_model_material_updates.c native/mh3_model_gpu_inputs.c
  native/mh3_model_material_prep.c
  PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
target_sources(MH3 PRIVATE src/native_resource_lifetime.cpp native/mh3_resource_lifetime.c)
target_sources(MH3 PRIVATE src/field_resources.cpp native/mh3_field_resources.c)
target_sources(MH3 PRIVATE src/model_pose.cpp native/mh3_model_pose.c)
target_sources(MH3 PRIVATE src/model_view.cpp native/mh3_model_view.c)
set_source_files_properties(native/mh3_model_view.c
  PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
target_sources(MH3 PRIVATE src/native_startup_textures.cpp native/mh3_startup_textures.c)
target_sources(MH3 PRIVATE src/native_system_fatal.cpp native/mh3_system_fatal.c)
target_sources(MH3 PRIVATE src/native_ui_cursor.cpp native/mh3_ui_cursor.c)
target_sources(MH3 PRIVATE src/model_vertex_sources.cpp native/mh3_vertex_sources.c)
target_sources(MH3 PRIVATE src/lighting.cpp native/mh3_lighting.c)
target_sources(MH3 PRIVATE src/native_frame.cpp)
target_sources(MH3 PRIVATE src/render_mode.cpp native/mh3_render_mode.c)
target_sources(MH3 PRIVATE src/native_display_utilities.cpp)
target_sources(MH3 PRIVATE native/mh3_draw_state.c)
target_sources(MH3 PRIVATE src/model_state.cpp src/model_scene_state.cpp native/mh3_model_state.c
  src/model_texture_matrices.cpp native/mh3_texture_matrices.c native/mh3_material_state.c
  native/mh3_material_group_publication.c
  src/native_ui.cpp native/mh3_ui.c
  src/native_system_ui.cpp native/mh3_system_ui.c
  src/native_effect_draw.cpp native/mh3_effect_draw.c native/mh3_particle_draw_state.c
  src/native_scene_feedback.cpp native/mh3_scene_feedback.c
  native/mh3_demo_menu.c
  src/native_scene_queue.cpp native/mh3_scene_queue.c src/scene_cull.cpp native/mh3_scene_cull.c
  src/launcher_support.cpp)
# Clang's precise float pragma is unsupported for the Vita target. Keep the
# CPU fog/light producer's range/finite checks and operation order intact.
set_source_files_properties(native/mh3_material_state.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
set_source_files_properties(native/mh3_scene_queue.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
set_source_files_properties(native/mh3_system_ui.c PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
target_include_directories(MH3 PRIVATE include ..)
# glibc's backtrace_symbols only names functions in the dynamic symbol table,
# so export ours for symbolized backtraces of fatal errors (and dladdr).
if(NOT VITA)
  set_target_properties(MH3 PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(MH3 PRIVATE ${CMAKE_DL_LIBS})
  # Vita pad / host stick mapping to the Classic Controller report.
  add_executable(mh3_classic_map_check ../tests/native/test_mh3_classic_map.c native/mh3_classic_map.c)
  target_include_directories(mh3_classic_map_check PRIVATE ..)
  target_link_libraries(mh3_classic_map_check PRIVATE m)
  add_test(NAME mh3_classic_map_check COMMAND mh3_classic_map_check)
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
    src/native_material.cpp src/model_gpu.cpp src/model_cull.cpp native/mh3_model_cull.c src/model_assets.cpp src/native_frame_statistics.cpp
    src/character_material.cpp src/native_shader_source.cpp native/mh3_shade.c src/native_frame_packet.cpp native/mh3_frame_state_exchange.c
    src/launcher_screen.cpp)
  if(VITA)
    target_sources(MH3 PRIVATE vita/audio_output.cpp)
  else()
    target_sources(MH3 PRIVATE src/audio_output.cpp)
  endif()
  if(MH3_ENABLE_EFB_COPY_TRACE)
    target_sources(MH3 PRIVATE src/efb_copy_trace.cpp)
    set_property(SOURCE src/graphics_host.cpp src/gx_stream.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_ENABLE_EFB_COPY_TRACE=1)
  endif()
  set_source_files_properties(src/graphics_host.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_render_state.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_display_utilities.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_texture_state.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/model_gpu.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_material.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/native_frame_packet.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  set_source_files_properties(src/launcher_screen.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  target_compile_definitions(MH3 PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3 PRIVATE aurora::core aurora::gx aurora::vi)
  if(NOT VITA)
    add_executable(mh3_native_frame_statistics_check
      ../tests/native/test_native_frame_statistics.cpp src/native_frame_statistics.cpp)
    target_include_directories(mh3_native_frame_statistics_check PRIVATE include)
    target_compile_features(mh3_native_frame_statistics_check PRIVATE cxx_std_20)
    add_test(NAME mh3_native_frame_statistics_check COMMAND mh3_native_frame_statistics_check)
    add_executable(mh3_fast_map_check ../tests/native/test_fast_map.cpp)
    target_include_directories(mh3_fast_map_check PRIVATE "${MH3_AURORA_ROOT}/include")
    target_compile_features(mh3_fast_map_check PRIVATE cxx_std_20)
    add_test(NAME mh3_fast_map_check COMMAND mh3_fast_map_check)
    add_executable(mh3_shader_cache_pack_check ../tests/native/test_shader_cache_pack.cpp)
    target_include_directories(mh3_shader_cache_pack_check PRIVATE "${MH3_AURORA_ROOT}/lib")
    target_compile_features(mh3_shader_cache_pack_check PRIVATE cxx_std_20)
    target_compile_options(mh3_shader_cache_pack_check PRIVATE -UNDEBUG)
    add_test(NAME mh3_shader_cache_pack_check COMMAND mh3_shader_cache_pack_check)
    # The Vita shader bank generator (docs/vita-shader-bank.md): sources of
    # every stage, keys unique, every fixed GLSL program translatable.
    add_executable(mh3-gxm-shader-bank ../scripts/vita_shader_bank.cpp src/native_shader_source.cpp "${MH3_AURORA_ROOT}/lib/gfx/cg_shader.cpp"
      "${MH3_AURORA_ROOT}/lib/gfx/shader_source.cpp" "${MH3_AURORA_ROOT}/lib/gfx/fixed_shader_sources.cpp")
    target_include_directories(mh3-gxm-shader-bank PRIVATE "${MH3_AURORA_ROOT}/lib" "${MH3_AURORA_ROOT}/include")
    target_compile_definitions(mh3-gxm-shader-bank PRIVATE TARGET_PC)
    target_compile_features(mh3-gxm-shader-bank PRIVATE cxx_std_20)
    target_link_libraries(mh3-gxm-shader-bank PRIVATE xxhash)
    add_test(NAME mh3_gxm_shader_bank_sources COMMAND mh3-gxm-shader-bank "${CMAKE_BINARY_DIR}/gxm-shader-bank-check"
      "${MH3_AURORA_ROOT}/lib/gxm/utility" --preflight)
    add_executable(mh3_shade_check ../tests/native/test_mh3_shade.c native/mh3_shade.c)
    target_include_directories(mh3_shade_check PRIVATE "${MH3_AURORA_ROOT}/include")
    target_link_libraries(mh3_shade_check PRIVATE m)
    target_compile_options(mh3_shade_check PRIVATE -UNDEBUG)
    add_test(NAME mh3_shade_check COMMAND mh3_shade_check)
    add_executable(mh3_native_depth_clear_check EXCLUDE_FROM_ALL
      ../tests/native/native_depth_clear_check.cpp src/native_display_utilities.cpp
      src/native_render_state.cpp native/mh3_draw_state.c)
    target_include_directories(mh3_native_depth_clear_check BEFORE PRIVATE
      ../tests/native/ui_cursor_test_support "${MH3_AURORA_ROOT}/lib" include ..)
    target_compile_features(mh3_native_depth_clear_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_depth_clear_check PRIVATE aurora::core aurora::gx aurora::vi)
    add_executable(mh3_native_geometry_check EXCLUDE_FROM_ALL
      "${MH3_AURORA_ROOT}/tests/native_geometry_check.cpp"
      src/native_render_state.cpp native/mh3_draw_state.c)
    target_include_directories(mh3_native_geometry_check PRIVATE "${MH3_AURORA_ROOT}/lib" include ..)
    target_compile_features(mh3_native_geometry_check PRIVATE cxx_std_20)
    target_link_libraries(mh3_native_geometry_check PRIVATE aurora::core aurora::gx aurora::vi)
    add_executable(mh3_native_material_check EXCLUDE_FROM_ALL
      ../tests/native/native_material_check.cpp src/native_material.cpp src/native_render_state.cpp
      src/native_shader_source.cpp native/mh3_shade.c native/mh3_material_state.c native/mh3_material_group_publication.c native/mh3_draw_state.c
      src/model_gpu.cpp native/mh3_model_gpu_inputs.c native/mh3_mesh_pack.c native/mh3_material_pack.c src/model_cull.cpp native/mh3_model_cull.c)
    target_include_directories(mh3_native_material_check PRIVATE "${MH3_AURORA_ROOT}/lib" include ..)
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
    vita/vita_controller.cpp src/launcher_disc.cpp src/launcher_textures.cpp)
  option(MH3_VITA_FAST_TLS "Use initialized VitaSDK TLS lookup fast paths" ON)
  if(MH3_VITA_FAST_TLS)
    target_sources(MH3 PRIVATE vita/fast_tls.c)
    target_link_options(MH3 PRIVATE "-Wl,--wrap=__getreent"
      "-Wl,--wrap=pte_osTlsGetValue")
  endif()
  # Drop unreferenced sections (also the nod bridge's Rust std helpers that
  # Vita newlib cannot provide).
  target_link_options(MH3 PRIVATE "-Wl,--gc-sections")
  set_source_files_properties(src/launcher_disc.cpp src/launcher_textures.cpp vita/launcher.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  if(MH3_DEVELOPMENT)
    set_property(SOURCE vita/platform.cpp vita/launcher.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
  endif()
  target_link_libraries(MH3 PRIVATE kubridge_stub SceAudio_stub ScePower_stub SceAppMgr_stub
    ScePvf_stub SceVshBridge_stub SceIme_stub SceSysmodule_stub SceCtrl_stub SceTouch_stub SceCommonDialog_stub)
  set_target_properties(MH3 PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vita/ldscript.ld")
  # Every GXM program, compiled offline (docs/vita-shader-bank.md).
  include(../cmake/GxmShaderBank.cmake)
  include(FetchContent)
  FetchContent_GetProperties(xxhash)
  mh3_gxm_shader_bank(MH3 "${xxhash_SOURCE_DIR}")
  set(VITA_MAKE_FSELF_FLAGS "-a 0x2800000000000001")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
  vita_create_self(eboot.bin MH3 UNSAFE NOASLR)
  vita_create_vpk(mh3-vita.vpk MH3WII001 eboot.bin VERSION 00.01 NAME "Monster Hunter Tri")
endif()

# Game data preparation: the native model stage on every platform; on Linux,
# --rom runs the same disc, texture and model stages as the Vita launcher.
target_sources(MH3 PRIVATE src/launcher_models.cpp src/rom_image.cpp src/asset_cache.cpp
  native/mh3_prepare_model.c native/mh3_prepare_corners.c native/mh3_prepare_writer.c)
set_source_files_properties(src/launcher_models.cpp src/asset_cache.cpp src/disc_image.cpp src/graphics_host.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
if(NOT VITA)
  target_sources(MH3 PRIVATE src/launcher_disc.cpp src/launcher_textures.cpp)
  set_source_files_properties(src/launcher_disc.cpp src/launcher_textures.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  # MH3 --launcher: the Vita launcher's menus on a development host.
  if(MH3_DEVELOPMENT AND TARGET aurora::core)
    target_sources(MH3 PRIVATE vita/launcher.cpp vita/launcher_font.cpp)
    set_property(SOURCE vita/launcher.cpp vita/launcher_font.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
  endif()
endif()

# Disc images (ISO, RVZ, WIA) decode with the native decoder
# (src/rom_decoder.cpp, docs/rom-decoder.md). MH3_ROM_DECODER_NOD builds the
# earlier nod Rust bridge (vita/rom_decoder, needs cargo) instead, for
# comparison. Linux may leave decoding out (MH3_ROM_DECODER=OFF): then only a
# decrypted ISO (nodtool convert --decrypt) can be read.
option(MH3_ROM_DECODER "Decode ISO/RVZ/WIA disc images on Linux (the Vita always decodes)" ON)
option(MH3_ROM_DECODER_NOD "Decode disc images with the nod Rust library instead (needs cargo)" OFF)
if(VITA OR MH3_ROM_DECODER)
  set(rom_decoder_dir "${CMAKE_CURRENT_SOURCE_DIR}/vita/rom_decoder")
  if(NOT MH3_ROM_DECODER_NOD)
    if(NOT TARGET libzstd_static)
      message(FATAL_ERROR "The ROM decoder needs Aurora's zstd (AURORA_CACHE_USE_ZSTD=ON)")
    endif()
    # libbzip2 1.0.8 and the 7-Zip LZMA SDK decoders, unmodified (third_party/).
    file(GLOB rom_decoder_bzip2 CONFIGURE_DEPENDS third_party/bzip2/*.c)
    add_library(mh3_rom_decoder STATIC src/rom_decoder.cpp ${rom_decoder_bzip2}
      third_party/lzma/LzmaDec.c third_party/lzma/Lzma2Dec.c)
    target_include_directories(mh3_rom_decoder PRIVATE third_party/bzip2 third_party/lzma)
    target_compile_definitions(mh3_rom_decoder PRIVATE BZ_NO_STDIO)
    set_source_files_properties(${rom_decoder_bzip2} PROPERTIES COMPILE_OPTIONS -w)
    target_link_libraries(mh3_rom_decoder PRIVATE libzstd_static)
    target_link_libraries(MH3 PRIVATE mh3_rom_decoder)
    if(VITA)
      # The EABI memory helpers the nod archive used to supply (vita/aeabi_memory.c).
      target_sources(MH3 PRIVATE vita/aeabi_memory.c)
      set_source_files_properties(vita/aeabi_memory.c PROPERTIES COMPILE_OPTIONS -fno-builtin)
    endif()
  elseif(VITA)
    set(rom_decoder_script "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-vita-rom-decoder.sh")
    set(rom_decoder_archive "${CMAKE_BINARY_DIR}/rom-decoder/armv7-sony-vita-newlibeabihf/release/libmh3_rom_decoder.a")
    add_custom_command(OUTPUT "${rom_decoder_archive}"
      COMMAND "${CMAKE_COMMAND}" -E env "VITASDK=${VITASDK}"
        "MH3_ROM_DECODER_UNWIND=${MH3_PROFILE_FRAME_POINTERS}"
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
  else()
    find_program(MH3_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|arm)")
      set(rom_decoder_triple armv7-unknown-linux-gnueabihf)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(rom_decoder_triple aarch64-unknown-linux-gnu)
    else()
      set(rom_decoder_triple x86_64-unknown-linux-gnu)
    endif()
    string(TOUPPER "${rom_decoder_triple}" rom_decoder_env)
    string(REPLACE "-" "_" rom_decoder_env "${rom_decoder_env}")
    string(REPLACE "-" "_" rom_decoder_cc "${rom_decoder_triple}")
    set(rom_decoder_archive "${CMAKE_BINARY_DIR}/rom-decoder/${rom_decoder_triple}/release/libmh3_rom_decoder.a")
    # The codec crates compile C with this build's compiler and target.
    add_custom_command(OUTPUT "${rom_decoder_archive}"
      COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rom-decoder"
        "CC_${rom_decoder_cc}=${CMAKE_C_COMPILER}" "CFLAGS_${rom_decoder_cc}=${CMAKE_C_FLAGS} -O2"
        "CARGO_TARGET_${rom_decoder_env}_LINKER=${CMAKE_C_COMPILER}"
        "${MH3_CARGO}" build --manifest-path "${rom_decoder_dir}/Cargo.toml" --locked --release
        --target ${rom_decoder_triple}
      DEPENDS "${rom_decoder_dir}/Cargo.toml" "${rom_decoder_dir}/Cargo.lock" "${rom_decoder_dir}/src/lib.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/vita/rom_decoder.h"
      COMMENT "Building the ROM decoder (${rom_decoder_triple})"
      VERBATIM)
    add_custom_target(MH3RomDecoderBuild DEPENDS "${rom_decoder_archive}")
    add_library(MH3RomDecoder STATIC IMPORTED)
    set_target_properties(MH3RomDecoder PROPERTIES IMPORTED_LOCATION "${rom_decoder_archive}")
    add_dependencies(MH3RomDecoder MH3RomDecoderBuild)
    target_link_libraries(MH3 PRIVATE MH3RomDecoder pthread m ${CMAKE_DL_LIBS})
  endif()
  set_property(SOURCE src/rom_image.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_HAS_ROM_DECODER=1)
endif()

if(MH3_WRITE_WATCH)
  target_sources(MH3 PRIVATE src/write_watch.cpp)
  target_compile_definitions(MH3 PRIVATE MH3_WRITE_WATCH=1)
endif()

target_sources(MH3 PRIVATE src/quest_catalog.cpp) # --list, --quest, launcher

# MH3_DEVELOPMENT applies to the runtime's own sources only, so toggling it
# does not rebuild the generated shards.
if(MH3_DEVELOPMENT)
  get_target_property(MH3_ALL_SOURCES MH3 SOURCES)
  list(FILTER MH3_ALL_SOURCES INCLUDE REGEX "^src/")
  set_property(SOURCE ${MH3_ALL_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
endif()

if(TARGET aurora::core)
  if(NOT VITA)
    option(MH3_BUILD_VITA_TEXTURE_PACKER "Build the offline Vita texture packer" ON)
    if(MH3_BUILD_VITA_TEXTURE_PACKER)
      find_file(MH3_STB_DXT_HEADER stb_dxt.h
        HINTS ENV STB_INCLUDE_DIR
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../../Vita3K/external/stb" /usr/include/stb
        DOC "External upstream stb_dxt.h for offline measured BC4/BC5 encoding")
      if(NOT MH3_STB_DXT_HEADER OR NOT EXISTS "${MH3_STB_DXT_HEADER}")
        message(FATAL_ERROR "Offline texture packer needs MH3_STB_DXT_HEADER; set its FILEPATH or disable MH3_BUILD_VITA_TEXTURE_PACKER")
      endif()
      add_executable(MH3VitaTexturePack ../scripts/vita_texture_pack.cpp)
      target_include_directories(MH3VitaTexturePack PRIVATE "${MH3_AURORA_ROOT}/lib")
      target_compile_definitions(MH3VitaTexturePack PRIVATE STB_DXT_HEADER="${MH3_STB_DXT_HEADER}")
      target_link_libraries(MH3VitaTexturePack PRIVATE "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx,aurora::vi>")
      add_executable(mh3_native_texture_pack_check EXCLUDE_FROM_ALL
        "${MH3_AURORA_ROOT}/tests/native_texture_pack_check.cpp")
      target_include_directories(mh3_native_texture_pack_check PRIVATE "${MH3_AURORA_ROOT}/lib")
      target_compile_features(mh3_native_texture_pack_check PRIVATE cxx_std_20)
      add_executable(mh3_native_texture_format_check EXCLUDE_FROM_ALL
        "${MH3_AURORA_ROOT}/tests/native_texture_format_check.cpp")
      target_include_directories(mh3_native_texture_format_check PRIVATE "${MH3_AURORA_ROOT}/lib")
      target_compile_features(mh3_native_texture_format_check PRIVATE cxx_std_20)
      target_link_libraries(mh3_native_texture_format_check PRIVATE aurora::core aurora::gx aurora::vi)
    endif()
    add_executable(MH3LauncherTests EXCLUDE_FROM_ALL src/launcher_tests.cpp
      src/launcher_support.cpp src/launcher_disc.cpp src/launcher_textures.cpp src/launcher_models.cpp src/disc_image.cpp
      src/rom_image.cpp src/asset_cache.cpp
      native/mh3_prepare_model.c native/mh3_prepare_corners.c native/mh3_prepare_writer.c native/mh3_model_resources.c
      native/mh3_vertex_sources.c native/mh3_mesh_pack.c native/mh3_material_pack.c native/mh3_model_catalog.c)
    target_include_directories(MH3LauncherTests PRIVATE include .. "${MH3_AURORA_ROOT}/lib")
    target_link_libraries(MH3LauncherTests PRIVATE "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx,aurora::vi>")
    if(TARGET MH3RomDecoder)
      target_link_libraries(MH3LauncherTests PRIVATE MH3RomDecoder pthread m ${CMAKE_DL_LIBS})
    endif()
  endif()
  add_executable(MH3ControllerHostTest src/controller_sdl_tests.cpp src/controller_host.cpp native/mh3_classic_map.c)
  target_include_directories(MH3ControllerHostTest PRIVATE include)
  target_compile_definitions(MH3ControllerHostTest PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3ControllerHostTest PRIVATE ${AURORA_SDL3_TARGET})
  add_test(NAME mh3_sdl_controller_test COMMAND MH3ControllerHostTest)
endif()
