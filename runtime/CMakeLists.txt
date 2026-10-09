cmake_minimum_required(VERSION 3.25)
project(MH3Recompiled LANGUAGES CXX ASM)
option(MH3_ENABLE_GUEST_CALL_TRACE "Enable temporary guest call graph instrumentation" OFF)
option(MH3_ENABLE_COLLISION_CHECK "Build isolated Dolphin collision counterexample checks" OFF)
option(MH3_ENABLE_GPU_VALIDATION "Keep Dawn API validation enabled for renderer checks" OFF)
option(MH3_ENABLE_GX_DIAGNOSTICS "Enable controlled renderer isolation and state logging" OFF)
# Supported targets: arm32 (armhf) and arm64 (aarch64) Linux, rendering
# through Dawn's Vulkan backend. MH3_ALLOW_UNSUPPORTED_HOST keeps other hosts
# configurable for development only; they are not a supported product.
option(MH3_ALLOW_UNSUPPORTED_HOST "Configure on a host other than arm32/arm64 Linux" OFF)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|armv7l|armv7|armv8l|arm)$")
  if(NOT MH3_ALLOW_UNSUPPORTED_HOST)
    message(FATAL_ERROR "MH3 targets arm32/arm64 Linux; this is ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
      "Cross-compile with cmake/toolchains/linux-arm32.cmake or linux-arm64.cmake, or set MH3_ALLOW_UNSUPPORTED_HOST=ON.")
  endif()
  message(WARNING "Configuring for unsupported host ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(MH3_AURORA_ROOT)
  enable_language(C)
  add_subdirectory("${MH3_AURORA_ROOT}" "${CMAKE_BINARY_DIR}/aurora")
  include(../scripts/aurora-efb.cmake)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The translated ISA helpers require Clang")
endif()
include(../generated/build_shards/shards.cmake)
add_executable(MH3
  src/main.cpp src/memory.cpp src/os_boot.cpp src/os_sleep.cpp src/os_alarm.cpp src/os_devices.cpp src/abi_bridge.cpp
  src/ppc_helpers.cpp src/fpu_helpers.cpp src/executable_guard.cpp src/rso_services.cpp src/wpad_services.cpp src/controller_host.cpp src/controller_tests.cpp src/village_boot.cpp src/audio_interface.cpp
  src/audio/ax_mix.cpp src/audio/ax_memory.cpp src/audio_services.cpp src/audio_tests.cpp src/audio_dma.cpp
  src/device_tests.cpp src/efb_host.cpp src/ios.cpp src/usb_hid_services.cpp src/disc_image.cpp src/video_interface.cpp src/gx_registers.cpp src/gx_stream.cpp src/gx_services.cpp src/vi_services.cpp
  ../generated/data_sections_init.cpp ../generated/data_sections_init_blobs.S
  ../generated/rso_overlays/map_village.cpp ../generated/rso_overlays/quest_field.cpp ../generated/rso_overlays/enemy_field.cpp ../generated/rso_overlays/map01_field.cpp
  ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
  ${MKW_BASE_REGISTRATION_SOURCES})
target_include_directories(MH3 PRIVATE include ..)
# glibc's backtrace_symbols only names functions in the dynamic symbol table,
# so export ours for symbolized MH3_TRACE_ERRORS stack traces.
set_target_properties(MH3 PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(MH3 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O1;-gline-tables-only;-ffp-contract=off>)
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
  target_sources(MH3 PRIVATE src/graphics_host.cpp src/audio_output.cpp)
  if(MH3_ENABLE_GX_DIAGNOSTICS)
    set_property(SOURCE src/graphics_host.cpp APPEND PROPERTY COMPILE_DEFINITIONS MH3_ENABLE_GX_DIAGNOSTICS=1)
    target_link_libraries(MH3 PRIVATE absl::flat_hash_map)
  endif()
  set_source_files_properties(src/graphics_host.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
  if(NOT DAWN_ENABLE_VULKAN)
    message(FATAL_ERROR "MH3 renders through Vulkan; the selected Dawn has no Vulkan backend")
  endif()
  target_compile_definitions(MH3 PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3 PRIVATE aurora::core aurora::gx aurora::vi dawn::webgpu_dawn)
endif()

if(TARGET aurora::core)
  add_executable(MH3ControllerHostTest src/controller_sdl_tests.cpp src/controller_host.cpp)
  target_include_directories(MH3ControllerHostTest PRIVATE include)
  target_compile_definitions(MH3ControllerHostTest PRIVATE MH3_HAS_AURORA=1)
  target_link_libraries(MH3ControllerHostTest PRIVATE ${AURORA_SDL3_TARGET})
  add_test(NAME mh3_sdl_controller_test COMMAND MH3ControllerHostTest)
endif()
