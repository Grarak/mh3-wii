# The WebGPU shader bank: every program the web build can
# bind, converted from its GLSL ES source to WGSL at build time and embedded.
#
#   mh3_web_shader_bank(TARGET XXHASH_DIR)
#
# 1. A host build of scripts/web_shader_bank.cpp writes the GLSL of every
#    stage (the native shader key domain and the fixed GLSL programs) and a
#    manifest.
# 2. scripts/web-shader-bank.py converts them with glslangValidator and naga,
#    caching each stage by tools, converter and source, and emits zstd-compressed
#    tables.
# 3. TARGET compiles the tables, which #embed the blob.
# Any change to the generator inputs regenerates the bank; unchanged stages
# come from the cache.
set(MH3_WEB_SHADER_BANK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "")
set(WEB_SHADER_BANK_CACHE "" CACHE PATH "Converted WGSL cache (default: ~/.cache/mh3-web-shader-bank)")

function(mh3_web_shader_bank target xxhash_dir)
  set(root "${MH3_WEB_SHADER_BANK_ROOT}")
  set(dir "${CMAKE_BINARY_DIR}/web-shader-bank")
  find_program(MH3_HOST_CXX NAMES clang++-21 clang++ c++ REQUIRED)
  find_program(MH3_ZSTD zstd REQUIRED)
  find_program(MH3_GLSLANG glslangValidator REQUIRED)
  find_program(MH3_NAGA naga HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(sources
    "${root}/scripts/web_shader_bank.cpp"
    "${root}/runtime/src/native_shader_source.cpp"
    "${root}/aurora-main/lib/gfx/shader_source.cpp"
    "${root}/aurora-main/lib/gfx/fixed_shader_sources.cpp")
  set(headers
    "${root}/runtime/src/native_shader_source.h"
    "${root}/runtime/native/mh3_shade_chain.h"
    "${root}/aurora-main/lib/gfx/shader_source.hpp"
    "${root}/aurora-main/lib/gfx/fixed_shader_sources.hpp"
    "${root}/aurora-main/lib/gfx/depth_peek_shaders.hpp"
    "${root}/aurora-main/lib/gfx/generated_texture_shaders.hpp"
    "${root}/aurora-main/lib/gfx/native_scene_feedback_shaders.hpp"
    "${root}/aurora-main/lib/gfx/tex_copy_conv_shaders.hpp"
    "${root}/aurora-main/lib/gfx/tex_copy_format_contract.hpp"
    "${root}/aurora-main/lib/gfx/tex_palette_conv_shaders.hpp"
    "${root}/aurora-main/lib/gx/reversed_z.hpp"
    "${root}/runtime/include/fatal.h")
  set(generator "${dir}/mh3-web-shader-bank")

  # The host compiler, not Emscripten's: the generator runs at build time.
  add_custom_command(OUTPUT "${generator}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
    COMMAND "${MH3_HOST_CXX}" -std=c++20 -O1 -DTARGET_PC -DMH3_WEB_SHADER_BANK "-I${root}/aurora-main/lib" "-I${root}/aurora-main/include"
      "-I${root}/runtime/include" "-I${xxhash_dir}" ${sources} -o "${generator}"
    DEPENDS ${sources} ${headers}
    COMMENT "Building the WebGPU shader bank generator (host)"
    VERBATIM)
  add_custom_command(OUTPUT "${dir}/sources/manifest.tsv"
    COMMAND "${generator}" "${dir}/sources"
    DEPENDS "${generator}"
    COMMENT "Writing the WebGPU shader bank sources"
    VERBATIM)
  set(cache)

  if(WEB_SHADER_BANK_CACHE)
    set(cache --cache "${WEB_SHADER_BANK_CACHE}")
  endif()

  set(outputs "${dir}/shader_bank_tables.cpp" "${dir}/shader_bank.bin" "${dir}/shader_bank.json")
  add_custom_command(OUTPUT ${outputs}
    COMMAND "${Python3_EXECUTABLE}" "${root}/scripts/web-shader-bank.py" "${dir}/sources" "${dir}"
      --glslang "${MH3_GLSLANG}" --naga "${MH3_NAGA}" ${cache}
    DEPENDS "${dir}/sources/manifest.tsv" "${root}/scripts/web-shader-bank.py"
    COMMENT "Converting the WebGPU shader bank to WGSL (glslang, naga)"
    USES_TERMINAL
    VERBATIM)
  target_sources(${target} PRIVATE "${dir}/shader_bank_tables.cpp")
  set_source_files_properties("${dir}/shader_bank_tables.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${root}/aurora-main/lib/webgpu" OBJECT_DEPENDS "${dir}/shader_bank.bin")
endfunction()
