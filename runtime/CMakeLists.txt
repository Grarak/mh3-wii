cmake_minimum_required(VERSION 3.25)
project(MH3Recompiled LANGUAGES CXX ASM)
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
  src/ppc_helpers.cpp src/fpu_helpers.cpp src/executable_guard.cpp src/rso_services.cpp src/wpad_services.cpp src/controller_host.cpp src/controller_tests.cpp src/audio_interface.cpp
  src/audio/ax_mix.cpp src/audio/ax_memory.cpp src/audio_services.cpp src/audio_tests.cpp src/audio_dma.cpp
  src/device_tests.cpp src/efb_host.cpp src/ios.cpp src/usb_hid_services.cpp src/disc_image.cpp src/video_interface.cpp src/gx_registers.cpp src/gx_stream.cpp src/gx_services.cpp src/vi_services.cpp
  ../generated/data_sections_init.cpp ../generated/data_sections_init_blobs.S
  ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
  ${MKW_BASE_REGISTRATION_SOURCES})
target_include_directories(MH3 PRIVATE include ..)
target_compile_options(MH3 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O1;-gline-tables-only;-ffp-contract=off>)
enable_testing()
add_test(NAME mh3_runtime_self_test COMMAND MH3 --self-test)
add_test(NAME mh3_audio_self_test COMMAND MH3 --audio-self-test)
if(TARGET aurora::core)
  target_sources(MH3 PRIVATE src/graphics_host.cpp src/audio_output.cpp)
  set_source_files_properties(src/graphics_host.cpp PROPERTIES INCLUDE_DIRECTORIES "${MH3_AURORA_ROOT}/lib")
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
