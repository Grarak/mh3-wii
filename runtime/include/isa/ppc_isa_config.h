#pragma once

#include <atomic>
#include <cstdint>

#define MKW_RESTRICT __restrict
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#else
#error "ppc_isa_config.h has no SIMD intrinsics header for this architecture"
#endif

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_WIN32)
#define MKW_PPC_FORCE_INLINE __forceinline
#define MKW_PPC_NO_INLINE __declspec(noinline)
#define MKW_PPC_INTERNAL_CALL __regcall
#else
// __forceinline/__declspec are MS-extension keywords Clang only recognizes when targeting
// Windows (MSVC or mingw); native Linux Clang needs the GNU-attribute spellings instead.
// __regcall has no portable non-Windows equivalent worth chasing here - the extra register
// args it saves matter for the hot PPC interpreter loop on Windows, but plain calls are fine
// elsewhere.
#define MKW_PPC_FORCE_INLINE __attribute__((always_inline)) inline
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_INTERNAL_CALL
#endif
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#define MKW_PPC_COLD __attribute__((cold))


using MkwStateFreeResult2 = uint64_t __attribute__((ext_vector_type(2)));
