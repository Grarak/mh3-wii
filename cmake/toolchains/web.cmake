# The web build: Emscripten's toolchain, with pthreads
# (SharedArrayBuffer) and native WebAssembly exceptions for every object, the
# runtime's and the fetched dependencies' alike. Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/web.cmake ...
# EMSDK names the Emscripten SDK (default ~/emsdk).
if(DEFINED ENV{EMSDK})
  set(mh3_emsdk "$ENV{EMSDK}")
else()
  set(mh3_emsdk "$ENV{HOME}/emsdk")
endif()

include("${mh3_emsdk}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

set(mh3_web_flags "-pthread -fwasm-exceptions")
set(CMAKE_C_FLAGS_INIT "${mh3_web_flags}")
set(CMAKE_CXX_FLAGS_INIT "${mh3_web_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${mh3_web_flags}")

# Every library links statically into the one module, also on a reconfigure
# (aurora-main/extern drops the BUILD_SHARED_LIBS cache entry).
set(BUILD_SHARED_LIBS OFF)
