cmake_minimum_required(VERSION 3.25)
project(MH3Recompiled LANGUAGES CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The translated ISA helpers require Clang")
endif()
include(../generated/build_shards/shards.cmake)
add_executable(MH3
  src/main.cpp src/memory.cpp src/os_boot.cpp src/os_devices.cpp src/abi_bridge.cpp
  src/ppc_helpers.cpp src/fpu_helpers.cpp src/executable_guard.cpp
  src/device_tests.cpp src/ios.cpp
  ../generated/data_sections_init.cpp ../generated/data_sections_init_blobs.S
  ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS}
  ${MKW_BASE_REGISTRATION_SOURCES})
target_include_directories(MH3 PRIVATE include ..)
target_compile_options(MH3 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O1;-gline-tables-only;-ffp-contract=off>)
enable_testing()
add_test(NAME mh3_runtime_self_test COMMAND MH3 --self-test)
