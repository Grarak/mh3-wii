# The GXM shader bank (docs/vita-shader-bank.md): every program the Vita build
# can bind, compiled offline at build time and embedded in the executable.
#
#   mh3_gxm_shader_bank(TARGET XXHASH_DIR [PREFLIGHT])
#
# 1. A host build of scripts/vita_shader_bank.cpp writes the Cg of every
#    stage (the native shader key domain, the fixed GLSL programs through the
#    runtime's translator, the utility Cg) and a manifest.
# 2. scripts/vita-shader-bank.py compiles them with psp2cgc (Docker image
#    psp2cgc-wine, Sony host tools in VITA_SONY_HOST_TOOLS), caching each
#    program by compiler, options and source, and emits zstd-compressed tables.
# 3. TARGET compiles the tables and the blob.
# Any change to the generator inputs regenerates the bank; unchanged sources
# reuse the cache, so only changed stages are recompiled.
set(MH3_GXM_SHADER_BANK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "")
set(GXM_SHADER_BANK_CACHE "" CACHE PATH "Compiled GXP cache (default: ~/.cache/mh3-gxm-shader-bank)")
set(VITA_SONY_HOST_TOOLS "/path/to/psp2-sdk/host_tools" CACHE PATH "Sony PS Vita SDK host_tools (psp2cgc.exe)")

function(mh3_gxm_shader_bank target xxhash_dir)
  cmake_parse_arguments(BANK "PREFLIGHT" "" "" ${ARGN})
  set(root "${MH3_GXM_SHADER_BANK_ROOT}")
  set(dir "${CMAKE_BINARY_DIR}/gxm-shader-bank")
  find_program(MH3_HOST_CXX NAMES clang++-21 clang++ c++ REQUIRED)
  find_program(MH3_ZSTD zstd REQUIRED)
  find_program(MH3_DOCKER docker REQUIRED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  if(NOT EXISTS "${VITA_SONY_HOST_TOOLS}/bin/psp2cgc.exe")
    message(FATAL_ERROR "The GXM shader bank needs psp2cgc.exe; set VITA_SONY_HOST_TOOLS")
  endif()
  set(sources
    "${root}/scripts/vita_shader_bank.cpp"
    "${root}/runtime/src/native_shader_source.cpp"
    "${root}/aurora-main/lib/gfx/cg_shader.cpp"
    "${root}/aurora-main/lib/gfx/shader_source.cpp"
    "${root}/aurora-main/lib/gfx/fixed_shader_sources.cpp")
  set(headers
    "${root}/runtime/src/native_shader_source.h"
    "${root}/aurora-main/lib/gfx/cg_shader.hpp"
    "${root}/aurora-main/lib/gfx/shader_source.hpp"
    "${root}/aurora-main/lib/gfx/fixed_shader_sources.hpp"
    "${root}/aurora-main/lib/gfx/depth_peek_shaders.hpp"
    "${root}/aurora-main/lib/gfx/generated_texture_shaders.hpp"
    "${root}/aurora-main/lib/gfx/native_scene_feedback_shaders.hpp"
    "${root}/aurora-main/lib/gfx/tex_copy_conv_shaders.hpp"
    "${root}/aurora-main/lib/gfx/tex_copy_format_contract.hpp"
    "${root}/aurora-main/lib/gfx/tex_palette_conv_shaders.hpp"
    "${root}/aurora-main/lib/gx/reversed_z.hpp"
    "${root}/aurora-main/lib/gxm/alpha_compare.hpp")
  file(GLOB utility CONFIGURE_DEPENDS "${root}/aurora-main/lib/gxm/utility/*.cg" "${root}/aurora-main/lib/gxm/utility/reference/*.cg")
  set(generator "${dir}/mh3-gxm-shader-bank")
  add_custom_command(OUTPUT "${generator}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
    COMMAND "${MH3_HOST_CXX}" -std=c++20 -O1 -DTARGET_PC "-I${root}/aurora-main/lib" "-I${root}/aurora-main/include" "-I${xxhash_dir}"
      ${sources} -o "${generator}"
    DEPENDS ${sources} ${headers}
    COMMENT "Building the GXM shader bank generator (host)"
    VERBATIM)
  set(mode)
  if(BANK_PREFLIGHT)
    set(mode --preflight)
  endif()
  add_custom_command(OUTPUT "${dir}/sources/manifest.tsv"
    COMMAND "${generator}" "${dir}/sources" "${root}/aurora-main/lib/gxm/utility" ${mode}
    DEPENDS "${generator}" ${utility}
    COMMENT "Writing the GXM shader bank sources"
    VERBATIM)
  set(cache)
  if(GXM_SHADER_BANK_CACHE)
    set(cache --cache "${GXM_SHADER_BANK_CACHE}")
  endif()
  set(outputs "${dir}/shader_bank_tables.cpp" "${dir}/shader_bank.bin" "${dir}/shader_bank.S" "${dir}/shader_bank.json")
  add_custom_command(OUTPUT ${outputs}
    COMMAND "${Python3_EXECUTABLE}" "${root}/scripts/vita-shader-bank.py" "${dir}/sources" "${dir}" --sdk "${VITA_SONY_HOST_TOOLS}" ${cache}
    DEPENDS "${dir}/sources/manifest.tsv" "${root}/scripts/vita-shader-bank.py"
    COMMENT "Compiling the GXM shader bank (psp2cgc)"
    USES_TERMINAL
    VERBATIM)
  target_sources(${target} PRIVATE "${dir}/shader_bank_tables.cpp" "${dir}/shader_bank.S")
  set_source_files_properties("${dir}/shader_bank_tables.cpp" PROPERTIES INCLUDE_DIRECTORIES "${root}/aurora-main/lib/gxm")
  set_source_files_properties("${dir}/shader_bank.S" PROPERTIES OBJECT_DEPENDS "${dir}/shader_bank.bin")
endfunction()
