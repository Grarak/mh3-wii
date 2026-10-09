// NW4R ef particle accessors the draw strategies call for each particle
// (effect_particle.cpp). Guest addresses in, host values out; each is also
// registered as the native port of its guest function.
#pragma once
#include "port_check.h"

#include <cstdint>

// nw4r::math SinCosRad (0x8009C7C4, msl_math.cpp) on host values.
void SinCosRadHost(double radians, float& sine, float& cosine);

// ef_util.cpp (msl_math.cpp) on guest buffers: the rotation matrix of XYZ
// radians (0x8009CA94), VEC3 normalize (0x8009C4E8, whether not zero) and a
// matrix's columns (0x8009CC04) or rows (0x8009CC84) scaled by a vector with
// zeros replaced.
void EfRotationMtx(uint32_t out, float x, float y, float z);
bool EfVecNormalize(uint32_t dst, uint32_t src);
void EfMtxScaleColumns(uint32_t out, uint32_t m, uint32_t scale);
void EfMtxScaleRows(uint32_t out, uint32_t scale, uint32_t m);

// The same on host floats: VEC3 normalize in place (0x8009C4E8; a zero
// vector is left), the sine lookup by 0x80798468's period (0x8009C754) and
// the rotation taking Y to a unit vector (0x8009B4AC).
bool EfNormalize(float v[3]);
void EfSinCosByPeriod(double radians, float& sine, float& cosine);
void EfMtxFromDirection(const float v[3], float m[12]);

// The random seed of a curve key's pick (0x8009EF58, ef_anim_curve.cpp).
uint32_t EfPickSeed(uint32_t particleSeed, uint32_t salt, uint32_t set, uint32_t loop);

namespace EffectParticle {
// Particle list of a ParticleManager (ut::List at +0x38, link offset at +0x42).
uint32_t First(uint32_t manager);
uint32_t Last(uint32_t manager);
uint32_t Next(uint32_t manager, uint32_t particle);
uint32_t Prev(uint32_t manager, uint32_t particle);

// The same, skipping particles whose life status (+0x0C) is not 1 (alive).
uint32_t FirstAlive(uint32_t manager);
uint32_t NextAlive(uint32_t manager, uint32_t particle);
uint32_t LastAlive(uint32_t manager);
uint32_t PrevAlive(uint32_t manager, uint32_t particle);

// The draw strategy's iteration functions (DrawStrategyImpl::GetGetFirstFunc
// and GetGetNextFunc results) as native functions; null for others.
using FirstFunction = uint32_t (*)(uint32_t manager);
using NextFunction = uint32_t (*)(uint32_t manager, uint32_t particle);
FirstFunction NativeFirst(uint32_t guest);
NextFunction NativeNext(uint32_t guest);

// Colour layer (0, 1), index (0 primary, 1 secondary): RGB, alpha.
void ColorRgb(uint32_t particle, uint32_t layer, uint32_t index, uint8_t out[3]);
uint8_t ColorAlpha(uint32_t particle, uint32_t layer, uint32_t index);

// The emitter's alpha flicker for the particle's age (255 without flicker).
uint32_t AlphaFlicker(uint32_t particle);

// The manager's colour modulation (+0x6C mode 1: constant, 2: by distance)
// applied to a primary and secondary RGBA colour in place; mode 0 leaves them.
void ModulateColors(uint32_t manager, uint32_t particle, uint8_t primary[4], uint8_t secondary[4]);

// Rotation in radians (+0x40) plus the random-range bytes and the manager's offset.
void Rotation(uint32_t particle, float out[3]);

// True when MH3_PORT_CHECK names any of these ports (development): callers
// then reach them through their guest addresses, where they are checked.
// Asked per particle, so the usual answer (MH3_PORT_CHECK not set) is inline.
bool PortCheckedSlow();
inline bool PortChecked() {
    return PortCheck::Set() && PortCheckedSlow();
}
} // namespace EffectParticle
