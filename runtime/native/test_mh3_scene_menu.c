#include "mh3_scene_feedback.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {
    BASE = 0x80000000u,
    BYTES = 0x02000000u,
    WORK = 0x81000000u,
    SCRATCH = 0x81700000u
};
typedef struct {
    uint8_t *memory;
    unsigned calls, begins, samples, wheels, objects, weapon_assignments, expected_slot;
    int active;
} spy;
static uint32_t read_value(void *context, uint32_t address, unsigned count) {
    spy *s = context;
    assert(address >= BASE && address - BASE <= BYTES - count);
    uint32_t value = 0;
    for (unsigned i = 0; i < count; i++)
        value = value << 8 | s->memory[address - BASE + i];
    return value;
}
static void write_value(void *context, uint32_t address, uint32_t value, unsigned count) {
    spy *s = context;
    assert(address >= BASE && address - BASE <= BYTES - count);
    for (unsigned i = 0; i < count; i++)
        s->memory[address - BASE + count - 1 - i] = (uint8_t)(value >> (i * 8));
}
static void put_float(spy *s, uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    write_value(s, address, bits, 4);
}
static void copy_value(void *context, uint32_t destination, uint32_t source, uint32_t count) {
    spy *s = context;
    assert(destination >= BASE && destination - BASE <= BYTES - count && source >= BASE && source - BASE <= BYTES - count);
    memcpy(s->memory + destination - BASE, s->memory + source - BASE, count);
}
static void zero_value(void *context, uint32_t address, uint32_t count) {
    spy *s = context;
    assert(address >= BASE && address - BASE <= BYTES - count);
    memset(s->memory + address - BASE, 0, count);
}
static uint32_t call_value(void *context, uint32_t function, const uint32_t *a, unsigned count) {
    spy *s = context;
    s->calls++;
    assert(count <= 8);
    if (function == 0x80040354) {
        put_float(s, a[0], 640);
        put_float(s, a[0] + 4, 448);
    }
    if (function == 0x8005b738 || function == 0x8005b75c)
        return 20;
    if (function == 0x802e05b4) {
        if (a[1]) {
            write_value(s, a[1], 10, 2);
            write_value(s, a[1] + 2, 20, 2);
        }
        return 0x81100000;
    }
    if (function == 0x802e0b38) {
        copy_value(s, a[0], 0x81100000, 36);
        write_value(s, a[3], 8, 2);
        write_value(s, a[3] + 2, 12, 2);
    }
    if (function == 0x801e68b4)
        copy_value(s, a[0], a[1], 36);
    if (function == 0x801f35ec || function == 0x801f3f40)
        return 1;
    if (function == 0x802e0e70 && a[0] == 0x805b926c) {
        if (s->active)
            s->wheels++;
    }
    if (function == 0x800d5168)
        return 0x81300000;
    if (function == 0x800474d4)
        return 0x81400000;
    if (function == 0x800988cc)
        return 1;
    if (function == 0x80223608)
        return 0x81500000;
    if (function == 0x800948d4)
        return 0x81101000 + a[1] * 32;
    if (function == 0x80223f54) {
        /* Resource selection must have an explicit slot even if a preceding
         * helper left a different value in the guest's volatile registers. */
        assert(count == 3 && a[0] == 0x81210000 && a[1] == s->expected_slot - 7 && a[2] == s->expected_slot);
        s->weapon_assignments++;
    }
    return 0;
}
static void string_copy(void *c, uint32_t d, uint32_t s) {
    (void)s;
    write_value(c, d, 0, 1);
}
static int string_compare(void *c, uint32_t a, uint32_t b) {
    (void)c;
    (void)a;
    (void)b;
    return 0;
}
static void texture_object(void *context, uint32_t object, uint32_t payload) {
    spy *s = context;
    assert(object >= 0x81101000 && object < 0x81101100 && payload == 0x81400000);
    s->objects++;
}
static void begin_sample(void *context, int16_t x, int16_t y) {
    spy *s = context;
    (void)x;
    (void)y;
    assert(!s->active);
    s->active = 1;
    s->begins++;
}
static int sample_value(void *context, int16_t x, int16_t y, uint8_t rgba[4]) {
    spy *s = context;
    (void)x;
    (void)y;
    assert(s->active);
    s->active = 0;
    s->samples++;
    rgba[0] = 123;
    rgba[1] = 45;
    rgba[2] = 67;
    rgba[3] = 255;
    return 1;
}
static float float_call(void *context, uint32_t function, const uint32_t *a, unsigned count, const float *f, unsigned floats) {
    (void)context;
    (void)a;
    assert(floats == 1 && count <= 1);
    assert(function == 0x80052364 || function == 0x80985644);
    assert(f[0] == f[0]);
    return 0;
}
int main(void) {
    spy s = {0};
    s.memory = calloc(1, BYTES);
    assert(s.memory);
    mh3_scene_cpu_ops ops = {&s, read_value, write_value, call_value, copy_value, zero_value, string_copy, string_compare, texture_object, begin_sample, sample_value, float_call};
    write_value(&s, 0x809d3568, WORK, 4);
    put_float(&s, 0x8065b4c0, 640);
    put_float(&s, 0x8065b4c4, 448);
    put_float(&s, 0x80993970, 640);
    put_float(&s, 0x8099393c, .5f);
    put_float(&s, 0x80993950, 60);
    put_float(&s, 0x8099397c, 60);
    write_value(&s, 0x8065b4bc, 640, 2);
    write_value(&s, 0x8065b4be, 448, 2);
    write_value(&s, 0x80993960, 0x43300000, 4);
    write_value(&s, 0x80993968, 0x43300000, 4);
    write_value(&s, 0x8099396c, 0x80000000, 4);
    /* Named pointer tables are external assets in the game; the spy supplies
     * valid CPU spans to exercise each menu control branch without a ROM. */
    const uint32_t tables[] = {0x809a7860, 0x809a7968, 0x809a7980, 0x809a7a70, 0x809a7a88, 0x809a7aa0, 0x809a7ae8, 0x809a7b00};
    for (unsigned i = 0; i < sizeof(tables) / 4; i++)
        write_value(&s, tables[i], 0x811f0000, 4);
    for (unsigned mode = 0; mode < 13; mode++)
        for (unsigned flag = 0; flag < 5; flag++) {
            write_value(&s, WORK + 0x10fa, mode, 1);
            write_value(&s, WORK + 0x10fb, flag, 1);
            write_value(&s, WORK + 0x10eb, 1, 1);
            write_value(&s, WORK + 0x10f6, 1, 1);
            const unsigned previous = s.samples;
            assert(mh3_scene_demo_character_menu(&ops, SCRATCH));
            assert(!s.active);
            if (flag == 1 && (mode == 5 || mode == 10)) {
                assert(s.samples == previous + 1);
                const uint32_t color = WORK + (mode == 5 ? 0x10e7 : 0x10f2);
                assert(read_value(&s, color, 1) == 123 && read_value(&s, color + 1, 1) == 45 && read_value(&s, color + 2, 1) == 67);
            }
        }
    write_value(&s, 0x805b8fb4, 0xfffd, 2);
    write_value(&s, WORK, 0, 1);
    write_value(&s, WORK + 2, 0, 1);
    assert(mh3_scene_inner_color_menu(&ops, WORK, SCRATCH));
    assert(read_value(&s, WORK + 0x14, 4) == 0x7b2d43ff && s.wheels == 1);
    write_value(&s, 0x805b8f68, 0xfffd, 2);
    write_value(&s, WORK, 1, 1);
    write_value(&s, WORK + 3, 0, 1);
    assert(mh3_scene_equipment_color_menu(&ops, WORK, SCRATCH));
    assert(read_value(&s, WORK + 0x14, 2) == ((123 & 248) << 8 | (45 & 252) << 3 | 67 >> 3) && s.wheels == 2);
    const uint32_t owner = 0x81200000, work = 0x81201000;
    write_value(&s, owner + 0x13c, 0x81210000, 4);
    write_value(&s, 0x80796fe8, 0x81220000, 4);
    write_value(&s, work + 0x9c, UINT32_MAX, 4);
    write_value(&s, work + 0x18, 1, 4);
    assert(mh3_scene_equipment_update(&ops, owner, work, 0, SCRATCH));
    assert(s.objects == 8 && read_value(&s, work + 0x18, 4) == UINT32_MAX && read_value(&s, work, 1) == 8);
    for (unsigned slot = 7; slot < 10; slot++) {
        s.expected_slot = slot;
        write_value(&s, work + slot * 4 + 0x9c, UINT32_MAX, 4);
        write_value(&s, work + slot * 4 + 0x18, 0x01000001, 4);
        assert(mh3_scene_equipment_update(&ops, owner, work, slot, SCRATCH));
        assert(read_value(&s, work + slot * 4 + 0x18, 4) == UINT32_MAX && read_value(&s, work + slot, 1) == 8);
    }
    assert(s.weapon_assignments == 3);
    assert(s.begins == s.samples && s.begins == 4);
    free(s.memory);
    puts("native CPU menus:65 demo branches,2 wheel selectors,8 typed equipment objects,3 explicit weapon resource slots pass");
    return 0;
}
