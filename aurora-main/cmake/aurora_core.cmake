add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/input.cpp
        lib/window.cpp
        lib/logging.cpp
        lib/system_info.cpp
        lib/system_info.hpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxhash)
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map sqlite3 TracyClient)
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
    target_sources(aurora_core PRIVATE lib/gl/device.cpp lib/gfx/port_capture.cpp lib/gfx/shader_source.cpp)
    target_link_libraries(aurora_core PUBLIC absl::any_invocable)
    if(AURORA_GXM)
      target_compile_definitions(aurora_core PUBLIC AURORA_GXM)
      target_compile_options(aurora_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-gline-tables-only>)
      target_sources(aurora_core PRIVATE lib/gxm/renderer.cpp lib/gxm/device.cpp lib/gxm/memory.cpp
        lib/gxm/draw_memory.cpp lib/gxm/texture.cpp lib/gxm/raster.cpp lib/gxm/shaders.cpp lib/gxm/shaders.S)
      set_source_files_properties(lib/gxm/shaders.S PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lib/gxm/shaders.bin")
      target_include_directories(aurora_core PRIVATE lib/gxm)
      target_link_libraries(aurora_core PUBLIC SceGxm_stub SceDisplay_stub)
    else()
      find_path(AURORA_GLES3_INCLUDE_DIR GLES3/gl3.h REQUIRED)
      find_library(AURORA_GLESV2_LIBRARY NAMES GLESv2 REQUIRED)
      target_sources(aurora_core PRIVATE lib/gl/gl.cpp)
      target_include_directories(aurora_core PUBLIC ${AURORA_GLES3_INCLUDE_DIR})
      target_link_libraries(aurora_core PUBLIC ${AURORA_GLESV2_LIBRARY})
    endif()
endif ()
