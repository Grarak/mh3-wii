# The web build's executable (docs/optimizations/platforms/web-build.md): MH3.js and MH3.wasm, a
# modularized Emscripten program that runs main() on a pthread (the game
# thread) while the browser's main thread owns the canvas, WebGPU and the
# render loop. runtime/web/index.html starts it.
include(../cmake/WebShaderBank.cmake)
include(FetchContent)
FetchContent_GetProperties(xxhash)
mh3_web_shader_bank(MH3 "${xxhash_SOURCE_DIR}")
target_sources(MH3 PRIVATE web/web_files.cpp)
set_target_properties(MH3 PROPERTIES SUFFIX ".js")

# Guest RAM is folded into linear memory below the static data
# (guest_flat_memory.h): 16 MiB to 336 MiB.
math(EXPR mh3_web_global_base "336 << 20")

target_link_options(MH3 PRIVATE
  -sPROXY_TO_PTHREAD=1
  -sMODULARIZE=1 -sEXPORT_NAME=createMH3
  -sENVIRONMENT=web,worker
  -sWASMFS=1
  -sGLOBAL_BASE=${mh3_web_global_base}
  -sINITIAL_MEMORY=2GB
  -sSTACK_SIZE=16MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB
  -sPTHREAD_POOL_SIZE=16
  -sMALLOC=mimalloc
  -sEXIT_RUNTIME=1
  -sEXPORTED_RUNTIME_METHODS=ENV
  --profiling-funcs)

# The page, its loader and the local server's headers come with the build.
foreach(file index.html mh3-web.js)
  configure_file(web/${file} "${CMAKE_BINARY_DIR}/${file}" COPYONLY)
endforeach()
