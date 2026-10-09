# Source patches for the Vita's fetched dependencies, applied in place once.
function(aurora_vita_replace file old new)
  set(_file "${file}")
  file(READ "${_file}" _source)
  set(_old "${old}")
  set(_new "${new}")
  string(FIND "${_source}" "${_new}" _patched)
  if(NOT _patched EQUAL -1)
    return()
  endif()
  string(FIND "${_source}" "${_old}" _anchor)
  if(_anchor EQUAL -1)
    message(FATAL_ERROR "Review the Vita dependency patch for ${_file}")
  endif()
  string(REPLACE "${_old}" "${_new}" _source "${_source}")
  file(WRITE "${_file}" "${_source}")
endfunction()

function(aurora_patch_vita_sdl source_dir)
  # SDL's timer, joystick and other internal workers belong on core 0 too.
  aurora_vita_replace("${source_dir}/src/thread/vita/SDL_systhread.c"
    "0,           // cpu affinity mask. 0 = all CPUs"
    "SCE_KERNEL_CPU_MASK_USER_0, // MH3: internal SDL workers stay on core 0")
endfunction()

function(aurora_patch_vita_fmt source_dir)
  # newlib declares explicit stream locking, but Vita's libc exports none.
  # Select fmt's fwrite path, which uses newlib's actual internal stream lock.
  aurora_vita_replace("${source_dir}/include/fmt/format-inl.h" [=[template <typename F>
struct has_flockfile<F, void_t<decltype(flockfile(&std::declval<F&>()))>>
    : std::true_type {};]=] [=[#ifndef __vita__
template <typename F>
struct has_flockfile<F, void_t<decltype(flockfile(&std::declval<F&>()))>>
    : std::true_type {};
#endif]=])
endfunction()
