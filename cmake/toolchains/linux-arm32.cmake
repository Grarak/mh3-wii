# Cross-compile MH3 for arm32 Linux (armhf: ARMv7-A hard-float EABI) with
# Clang 21, tuned for Cortex-A9. Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm32.cmake ...
# Set MH3_SYSROOT to a target root filesystem; without it the host's armhf
# multiarch packages are used (see docker/armhf-cross.Dockerfile).
# MH3_ARM32_CPU / MH3_ARM32_FPU override the defaults (cortex-a9 / neon, i.e.
# VFPv3-D32 + NEON; use vfpv3-d16 for an A9 without NEON). VFPv3 has no fused
# multiply-add: the float fused ops are computed exactly in double
# (PpcFmaFloatViaDoubleInline) and the double ones call libm's fma.
# MH3_CLANG selects the compiler (default clang-21). --target is repeated in
# the flags because some dependency probes (libpng genout) call the compiler
# with CMAKE_C_FLAGS but without CMAKE_C_COMPILER_TARGET. Debian armhf is
# built with 64-bit time_t/off_t (the t64 ABI), so match it.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(mh3_triple arm-linux-gnueabihf)
if(DEFINED ENV{MH3_CLANG})
  set(mh3_clang "$ENV{MH3_CLANG}")
else()
  set(mh3_clang clang-21)
endif()
set(CMAKE_C_COMPILER ${mh3_clang})
string(REGEX REPLACE "clang(-[0-9]+)?$" "clang++\\1" mh3_clangxx "${mh3_clang}")
set(CMAKE_CXX_COMPILER ${mh3_clangxx})
set(CMAKE_ASM_COMPILER ${mh3_clang})
set(CMAKE_C_COMPILER_TARGET ${mh3_triple})
set(CMAKE_CXX_COMPILER_TARGET ${mh3_triple})
set(CMAKE_ASM_COMPILER_TARGET ${mh3_triple})
set(CMAKE_LIBRARY_ARCHITECTURE ${mh3_triple})
set(mh3_arm32_cpu cortex-a9)
set(mh3_arm32_fpu neon)
if(DEFINED ENV{MH3_ARM32_CPU})
  set(mh3_arm32_cpu "$ENV{MH3_ARM32_CPU}")
endif()
if(DEFINED ENV{MH3_ARM32_FPU})
  set(mh3_arm32_fpu "$ENV{MH3_ARM32_FPU}")
endif()
set(mh3_arm32_flags "--target=${mh3_triple} -mcpu=${mh3_arm32_cpu} -mfpu=${mh3_arm32_fpu} -mfloat-abi=hard -mthumb -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64")
set(CMAKE_C_FLAGS_INIT "${mh3_arm32_flags}")
set(CMAKE_CXX_FLAGS_INIT "${mh3_arm32_flags}")
set(CMAKE_ASM_FLAGS_INIT "${mh3_arm32_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
if(DEFINED ENV{MH3_SYSROOT})
  set(CMAKE_SYSROOT "$ENV{MH3_SYSROOT}")
endif()
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/${mh3_triple}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
