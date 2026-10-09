/* EABI memory helpers (__aeabi_memcpy/memmove/memset/memclr and their 4/8
 * forms) for the Vita build, ported from compiler-builtins 0.1.160
 * (src/arm.rs and src/mem/impls.rs; Copyright (c) The Rust Project
 * contributors; MIT AND Apache-2.0 WITH LLVM-exception, see
 * toolchain/THIRD-PARTY-NOTICES.md).
 *
 * The nod ROM decoder's Rust archive supplied these to the whole program
 * (docs/vita-memory-helper-selection.md). Without them the SDK's newlib ones
 * would apply: a byte loop for memmove and a scalar loop for memset. This
 * keeps the helpers the game was profiled with. The public memcpy,
 * memmove and memset stay the SDK's routes to sceClib.
 *
 * Compiled with -fno-builtin, so the loops are not turned back into calls. */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((may_alias)) word_t;
typedef uint16_t __attribute__((may_alias)) half_t;
enum { WORD = 4, MASK = WORD - 1, THRESHOLD = 16 };
/* compiler-builtins' generic bodies are scalar loops; only the aligned 4/8
 * entries vectorize (with d16/d17, as there). */
#define SCALAR _Pragma("clang loop vectorize(disable) interleave(disable) unroll(disable)")

/* Bytes [0, size) of the aligned word at src, the rest zero (size < 4). */
static inline uint32_t load_partial(const uint8_t *src, size_t size)
{
    uint32_t out = 0;
    size_t at = 0;
    if (size & 2) {
        out = *(const half_t *)src;
        at = 2;
    }
    if (size & 1)
        out |= (uint32_t)src[at] << (at * 8);
    return out;
}

/* The last `size` bytes of the aligned word at src, in place, the rest zero. */
static inline uint32_t load_end_partial(const uint8_t *src, size_t size)
{
    const uint8_t *from = src + WORD - size;
    uint32_t out = 0;
    size_t at = 0;
    if (size & 1) {
        out = (uint32_t)from[0] << ((WORD - size) * 8);
        at = 1;
    }
    if (size & 2)
        out |= (uint32_t)*(const half_t *)(from + at) << ((WORD - size + at) * 8);
    return out;
}

static inline void copy_forward(uint8_t *dest, const uint8_t *src, size_t n)
{
    if (n >= THRESHOLD) {
        const size_t head = (size_t)(-(uintptr_t)dest) & MASK;
        SCALAR
        for (uint8_t *end = dest + head; dest < end;)
            *dest++ = *src++;
        n -= head;
        const size_t words = n & ~(size_t)MASK;
        word_t *d = (word_t *)dest;
        word_t *const end = (word_t *)(dest + words);
        const size_t offset = (uintptr_t)src & MASK;
        if (__builtin_expect(offset == 0, 1)) {
            const word_t *s = (const word_t *)src;
            SCALAR
            while (d < end)
                *d++ = *s++;
        } else {
            /* Aligned source words, reassembled by shifting. */
            const unsigned shift = (unsigned)offset * 8;
            const word_t *s = (const word_t *)(src - offset);
            uint32_t prev = load_end_partial((const uint8_t *)s, WORD - offset);
            SCALAR
            while (d + 1 < end) {
                const uint32_t cur = *++s;
                *d++ = prev >> shift | cur << (32 - shift);
                prev = cur;
            }
            const uint32_t cur = load_partial((const uint8_t *)(s + 1), offset);
            *d = prev >> shift | cur << (32 - shift);
        }
        dest += words;
        src += words;
        n -= words;
    }
    SCALAR
    for (uint8_t *end = dest + n; dest < end;)
        *dest++ = *src++;
}

static inline void copy_backward(uint8_t *dest, const uint8_t *src, size_t n)
{
    dest += n;
    src += n;
    if (n >= THRESHOLD) {
        const size_t tail = (uintptr_t)dest & MASK;
        SCALAR
        for (uint8_t *start = dest - tail; start < dest;)
            *--dest = *--src;
        n -= tail;
        const size_t words = n & ~(size_t)MASK;
        word_t *d = (word_t *)dest;
        word_t *const start = (word_t *)(dest - words);
        const size_t offset = (uintptr_t)src & MASK;
        if (__builtin_expect(offset == 0, 1)) {
            const word_t *s = (const word_t *)src;
            SCALAR
            while (start < d)
                *--d = *--s;
        } else {
            const unsigned shift = (unsigned)offset * 8;
            const word_t *s = (const word_t *)(src - offset);
            uint32_t prev = load_partial((const uint8_t *)s, offset);
            SCALAR
            while (start + 1 < d) {
                const uint32_t cur = *--s;
                *--d = prev << (32 - shift) | cur >> shift;
                prev = cur;
            }
            const uint32_t cur = load_end_partial((const uint8_t *)(s - 1), WORD - offset);
            *--d = prev << (32 - shift) | cur >> shift;
        }
        dest -= words;
        src -= words;
        n -= words;
    }
    SCALAR
    for (uint8_t *start = dest - n; start < dest;)
        *--dest = *--src;
}

static inline void set_bytes(uint8_t *s, uint8_t c, size_t n)
{
    if (__builtin_expect(n >= THRESHOLD, 1)) {
        const size_t head = (size_t)(-(uintptr_t)s) & MASK;
        SCALAR
        for (uint8_t *end = s + head; s < end;)
            *s++ = c;
        n -= head;
        const size_t words = n & ~(size_t)MASK;
        const uint32_t broadcast = c * 0x01010101u;
        SCALAR
        for (word_t *w = (word_t *)s, *end = (word_t *)(s + words); w < end;)
            *w++ = broadcast;
        s += words;
        n -= words;
    }
    SCALAR
    for (uint8_t *end = s + n; s < end;)
        *s++ = c;
}

void __aeabi_memcpy(void *dest, const void *src, size_t n)
{
    copy_forward(dest, src, n);
}

void __aeabi_memcpy4(void *dest, const void *src, size_t n)
{
    word_t *d = dest;
    const word_t *s = src;
    for (; n >= 4; n -= 4)
        *d++ = *s++;
    __aeabi_memcpy(d, s, n);
}

/* The 8-byte forms share their 4-byte form's code, as in compiler-builtins. */
void __aeabi_memcpy8(void *dest, const void *src, size_t n) __attribute__((alias("__aeabi_memcpy4")));

void __aeabi_memmove(void *dest, const void *src, size_t n)
{
    /* Forwards when dest is far enough ahead of src, or src is ahead of dest. */
    if ((uintptr_t)dest - (uintptr_t)src >= n)
        copy_forward(dest, src, n);
    else
        copy_backward(dest, src, n);
}

void __aeabi_memmove4(void *dest, const void *src, size_t n) __attribute__((alias("__aeabi_memmove")));
void __aeabi_memmove8(void *dest, const void *src, size_t n) __attribute__((alias("__aeabi_memmove")));

void __aeabi_memset(void *dest, size_t n, int c)
{
    set_bytes(dest, (uint8_t)c, n);
}

void __aeabi_memset4(void *dest, size_t n, int c)
{
    word_t *d = dest;
    const uint32_t broadcast = (uint8_t)c * 0x01010101u;
    for (; n >= 4; n -= 4)
        *d++ = broadcast;
    __aeabi_memset(d, n, (uint8_t)c);
}

void __aeabi_memset8(void *dest, size_t n, int c) __attribute__((alias("__aeabi_memset4")));

void __aeabi_memclr(void *dest, size_t n)
{
    __aeabi_memset(dest, n, 0);
}

void __aeabi_memclr4(void *dest, size_t n)
{
    __aeabi_memset4(dest, n, 0);
}

void __aeabi_memclr8(void *dest, size_t n) __attribute__((alias("__aeabi_memclr4")));
