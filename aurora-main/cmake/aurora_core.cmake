add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/input.cpp
        lib/window.cpp
        lib/logging.cpp
        lib/legacy_gx.cpp
        lib/system_info.cpp
        lib/system_info.hpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxhash)
target_link_libraries(aurora_core PRIVATE sqlite3)
if (AURORA_ENABLE_GX AND AURORA_CACHE_USE_ZSTD)
    target_compile_definitions(aurora_core PRIVATE AURORA_CACHE_USE_ZSTD)
    target_link_libraries(aurora_core PRIVATE libzstd_static)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    # stuff for fetching system info.
    target_link_libraries(aurora_core PRIVATE ntdll dxgi advapi32 user32)
elseif (APPLE)
    target_sources(aurora_core PRIVATE lib/system_info_mac.mm)
endif ()

if (AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/imgui.cpp)
    target_link_libraries(aurora_core PUBLIC imgui)
endif ()

if (AURORA_ENABLE_GX)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/gl/device.cpp lib/gfx/port_capture.cpp lib/gfx/cg_shader.cpp lib/gfx/shader_source.cpp)
    if(AURORA_GXM)
      target_compile_definitions(aurora_core PUBLIC AURORA_GXM)
      target_compile_options(aurora_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-gline-tables-only>)
      # The executable links the generated shader bank tables and blob
      # (cmake/GxmShaderBank.cmake, docs/vita-shader-bank.md).
      target_sources(aurora_core PRIVATE lib/gxm/renderer.cpp lib/gxm/device.cpp lib/gxm/memory.cpp
        lib/gxm/razor.cpp lib/gxm/draw_memory.cpp lib/gxm/texture.cpp lib/gxm/native_assets.cpp lib/gxm/raster.cpp lib/gxm/shader_bank.cpp)
      if(NOT TARGET libzstd_static)
        message(FATAL_ERROR "The GXM shader bank decompresses with zstd; enable AURORA_CACHE_USE_ZSTD")
      endif()
      target_link_libraries(aurora_core PRIVATE libzstd_static)
      if(MH3_DEVELOPMENT)
        # Razor captures and live metrics (MH3_RAZOR) are a development tool,
        # as is the runtime compiler that serves (and reports) bank misses.
        target_sources(aurora_core PRIVATE lib/gxm/runtime_shader.cpp)
        set_property(SOURCE lib/gxm/razor.cpp lib/gxm/device.cpp lib/gxm/renderer.cpp lib/gxm/runtime_shader.cpp
          APPEND PROPERTY COMPILE_DEFINITIONS MH3_DEVELOPMENT=1)
        target_link_libraries(aurora_core PUBLIC vitashark SceShaccCgExt SceShaccCg_stub_weak SceKernelModulemgr_stub taihen_stub)
      endif()
      target_include_directories(aurora_core PRIVATE lib/gxm)
      target_link_libraries(aurora_core PUBLIC SceGxm_stub SceDisplay_stub SceCommonDialog_stub
        SceRazorCapture_stub_weak SceRazorHud_stub_weak ScePamgr_stub)
    else()
      find_path(AURORA_GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
      find_library(AURORA_GLESV2_LIBRARY NAMES GLESv2 REQUIRED)
      target_sources(aurora_core PRIVATE lib/gl/gl.cpp)
      target_include_directories(aurora_core PUBLIC ${AURORA_GLES3_INCLUDE_DIR})
      target_link_libraries(aurora_core PUBLIC ${AURORA_GLESV2_LIBRARY})
    endif()
endif ()
