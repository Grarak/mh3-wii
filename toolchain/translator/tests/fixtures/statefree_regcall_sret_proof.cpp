#include <cstdint>
#include <cstdio>

#if defined(__clang__) && defined(_WIN64)
#define INTERNAL_CALL __regcall
#else
#define INTERNAL_CALL
#endif

struct Result15 {
    uint32_t r0, r1, r3, r4, r5, r6, r7, r8;
    uint32_t r9, r10, r11, r12, r30, r31, cr;
};

extern "C" __declspec(noinline) Result15 INTERNAL_CALL callee(
    uint32_t r1, uint32_t r3, uint32_t r4, uint32_t r5,
    uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9,
    uint32_t r10, uint32_t r11, uint32_t r12, uint32_t r30,
    uint32_t r31, uint32_t cr, uint32_t xer, uint32_t lr) {
    return {lr, r1, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r30, r31, cr ^ xer};
}

extern "C" __declspec(noinline) Result15 INTERNAL_CALL recurse(
    uint32_t depth, uint32_t pointer, uint32_t salt) {
    if (depth == 0) {
        return callee(0x80398be8u, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                      30, pointer, 0x44000088u, salt, 0x80124d08u);
    }
    auto nested = recurse(depth - 1, pointer, salt);
    return callee(nested.r1, nested.r3, nested.r4, nested.r5, nested.r6,
                  nested.r7, nested.r8, nested.r9, nested.r10, nested.r11,
                  nested.r12, nested.r30, nested.r31, nested.cr, salt,
                  nested.r0);
}

int main() {
    constexpr uint32_t expected = 0x802f12ccu;
    for (uint32_t i = 0; i < 1000000; ++i) {
        const auto result = recurse(4, expected, i);
        if (result.r31 != expected) {
            std::printf("mismatch iteration=%u expected=%08X actual=%08X\n", i, expected, result.r31);
            return 1;
        }
    }
    std::puts("statefree regcall+sret proof passed");
    return 0;
}
