# The web build's executable: MH3.js and MH3.wasm, a
# modularized Emscripten program that runs main() on a pthread (the game
# thread) while the browser's main thread owns the canvas, WebGPU and the
# render loop. runtime/web/index.html starts it.
include(../cmake/WebShaderBank.cmake)
include(FetchContent)
FetchContent_GetProperties(xxhash)
mh3_web_shader_bank(MH3 "${xxhash_SOURCE_DIR}")
target_sources(MH3 PRIVATE web/web_files.cpp)
set_target_properties(MH3 PROPERTIES SUFFIX ".js")

# The launcher's font (launcher_font.cpp reads a system sans font on hosts):
# the build machine's, preloaded at the same path into MH3.data. Liberation
# Sans (SIL OFL) and DejaVu Sans (Bitstream Vera) may be redistributed.
find_file(MH3_WEB_LAUNCHER_FONT NAMES LiberationSans-Regular.ttf DejaVuSans.ttf
  PATHS /usr/share/fonts/truetype/liberation /usr/share/fonts/truetype/dejavu NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT MH3_WEB_LAUNCHER_FONT)
  message(FATAL_ERROR "The web launcher needs Liberation Sans or DejaVu Sans (apt install fonts-liberation)")
endif()

target_link_options(MH3 PRIVATE "SHELL:--preload-file ${MH3_WEB_LAUNCHER_FONT}@${MH3_WEB_LAUNCHER_FONT}")
set_property(TARGET MH3 APPEND PROPERTY LINK_DEPENDS "${MH3_WEB_LAUNCHER_FONT}")

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
  # ENV for the page's environment, the string helpers for the launcher's
  # text prompt.
  -sEXPORTED_RUNTIME_METHODS=ENV,UTF8ToString,stringToUTF8
  --profiling-funcs)

# The page and its loader come with the build. The loader takes command-line
# arguments from the page's address in development builds only.
if(MH3_DEVELOPMENT)
  set(MH3_WEB_DEVELOPMENT true)
else()
  set(MH3_WEB_DEVELOPMENT false)
endif()

configure_file(web/index.html "${CMAKE_BINARY_DIR}/index.html" COPYONLY)
configure_file(web/mh3-web.js "${CMAKE_BINARY_DIR}/mh3-web.js" @ONLY)
configure_file(web/isolation.js "${CMAKE_BINARY_DIR}/isolation.js" COPYONLY)
configure_file(web/quest-form.js "${CMAKE_BINARY_DIR}/quest-form.js" COPYONLY)
