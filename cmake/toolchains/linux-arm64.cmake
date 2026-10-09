# Cross-compile MH3 for arm64 (aarch64) Linux with Clang.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake ...
# Set MH3_SYSROOT to a target root filesystem (Vulkan/X11/Wayland/ALSA headers
# and libraries); without it the host's multiarch cross packages are used.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(mh3_triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${mh3_triple})
set(CMAKE_CXX_COMPILER_TARGET ${mh3_triple})
set(CMAKE_ASM_COMPILER_TARGET ${mh3_triple})
set(CMAKE_LIBRARY_ARCHITECTURE ${mh3_triple})

if(DEFINED ENV{MH3_SYSROOT})
  set(CMAKE_SYSROOT "$ENV{MH3_SYSROOT}")
endif()

set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/${mh3_triple}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
