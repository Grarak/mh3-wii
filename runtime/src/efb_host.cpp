#include "efb_host.h"
#include "fatal.h"
#include "gx_panic.h"
#include "recomp_mod_loader.h"
#include "isa/ppc_isa_context.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <tuple>

bool DecodeEfbColorAddress(uint32_t address, uint16_t& x, uint16_t& y) {
    if ((address >> 30) == 1 || (address & 0x3FC00003u) != 0x08000000u)
        return false;

    x = (address >> 2) & 1023;
    y = (address >> 12) & 1023;
    return true;
}

uint32_t ConvertEfbArgb(uint32_t color, unsigned pixelFormat, unsigned alphaMode) {
    unsigned a = color >> 24, r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    auto six = [](unsigned c) {
        return (c & 252) | (c >> 6);
    };

    auto five = [](unsigned c) {
        return (c & 248) | (c >> 5);
    };

    if (pixelFormat == 1) {
        a = six(a);
        r = six(r);
        g = six(g);
        b = six(b);
    } else if (pixelFormat == 2) {
        a = 255;
        r = five(r);
        g = six(g);
        b = five(b);
    } else if (pixelFormat == 0)
        a = 255;
    else
        MH3_THROW(std::runtime_error("Unsupported EFB peek pixel format"));

    if (alphaMode == 0)
        a = 0;
    else if (alphaMode == 1)
        a = 255;
    else if (alphaMode != 2)
        MH3_THROW(std::runtime_error("Invalid PE alpha-read mode"));

    return (a << 24) | (r << 16) | (g << 8) | b;
}

bool ReadEfbColor32(uint32_t address, uint32_t& value) {
    uint16_t x, y;

    if (!DecodeEfbColorAddress(address, x, y))
        return false;

    PanicLegacyGx("GX CPU EFB color read", address);

    // The address encoding has ten bits per coordinate; the physical EFB is smaller.
    value = x < 640 && y < 528 ? ReadHostEfbColor(x, y) : 0;
    return true;
}

bool WriteEfbColor32(uint32_t address, uint32_t value) {
    uint16_t x, y;

    if (!DecodeEfbColorAddress(address, x, y))
        return false;

    PanicLegacyGx("GX CPU EFB color write", address);

    if (x < 640 && y < 528)
        WriteHostEfbColor(x, y, value);

    return true;
}
#ifndef MH3_HAS_AURORA
void WriteHostEfbColor(uint16_t, uint16_t, uint32_t) {
    MH3_THROW(std::runtime_error("EFB color writes require the Aurora-linked build"));
}

uint32_t ReadHostEfbColor(uint16_t, uint16_t) {
    MH3_THROW(std::runtime_error("EFB color reads require the Aurora-linked build"));
}
#endif
