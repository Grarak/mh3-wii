# Vita uses ELF containers but does not expose glibc's dynamic-loader ABI.
# Keep the host implementation and use Abseil's existing fallback on Vita.
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

function(aurora_patch_vita_tracy source_dir)
  set(_system "${source_dir}/public/common/TracySystem.cpp")
  aurora_vita_replace("${_system}" "#include \"TracySystem.hpp\"" [=[#include "TracySystem.hpp"
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif]=])
  aurora_vita_replace("${_system}" "#elif defined __APPLE__\n    uint64_t id;" [=[#elif defined __vita__
    return uint32_t( sceKernelGetThreadId() );
#elif defined __APPLE__
    uint64_t id;]=])
  aurora_vita_replace("${_system}"
    "#elif defined _GNU_SOURCE && !defined __EMSCRIPTEN__"
    "#elif defined _GNU_SOURCE && !defined __EMSCRIPTEN__ && !defined __vita__")
endfunction()

function(aurora_patch_vita_abseil source_dir)
  aurora_vita_replace("${source_dir}/absl/debugging/internal/elf_mem_image.h"
    "!defined(__VXWORKS__) && !defined(__hexagon__)"
    "!defined(__VXWORKS__) && !defined(__hexagon__) && !defined(__vita__)")

  # Vita newlib has no tm_gmtoff/tm_zone members. Its two TZ rules already
  # contain the standard and daylight offsets (seconds west of UTC).
  set(_tz "${source_dir}/absl/time/internal/cctz/src/time_zone_libc.cc")
  aurora_vita_replace("${_tz}" "#include <utility>" [=[#include <utility>

#if defined(__vita__)
extern "C" {
#include <sys/_tz_structs.h>
}
#endif]=])
  aurora_vita_replace("${_tz}" "#elif defined(__sun) || defined(_AIX)" [=[#elif defined(__vita__)
long tm_gmtoff(const std::tm& tm) {
  return -__gettzinfo()->__tzrule[tm.tm_isdst > 0].offset;
}
const char* tm_zone(const std::tm& tm) {
  return tzname[tm.tm_isdst > 0];
}
#elif defined(__sun) || defined(_AIX)]=])
  aurora_vita_replace("${_tz}"
    "al.offset = static_cast<int>(tm_gmtoff(*tmp));" [=[#if defined(__vita__)
  al.offset = local_ ? static_cast<int>(tm_gmtoff(*tmp)) : 0;
#else
  al.offset = static_cast<int>(tm_gmtoff(*tmp));
#endif]=])
endfunction()
