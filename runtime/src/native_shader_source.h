#pragma once
// Fixed native shaders, written by hand for GLES and GXM (see the .cpp).
#include <cstdint>
#include <string>

enum class NativeShaderVertex : uint8_t {
    Mesh,
    Geometry
};
enum class NativeShaderModel : uint8_t {
    Basic,
    Modulate
}; // MH3_SHADE_BASIC / MH3_SHADE_MODULATE
// Mesh texgen per role, fixed at compile time. Identity and Affine read the
// role's coordinate attribute (no projection, normalize, post or palette).
// Spot: colour-only spot lighting with clamped diffuse over `lights` lights;
// General: any GX channel configuration.
enum class NativeShaderLighting : uint8_t {
    None,
    Spot,
    General
};
enum class NativeShaderTexgen : uint8_t {
    None,
    Identity,
    Affine,
    General
};

struct NativeShaderVariant {
    // Vertex stage
    NativeShaderVertex vertex = NativeShaderVertex::Mesh;
    NativeShaderLighting lighting = NativeShaderLighting::None;
    uint8_t lights = 0;                // Spot only
    NativeShaderTexgen texgen[3] = {}; // Mesh only
    // Fragment stage
    NativeShaderModel model = NativeShaderModel::Modulate;
    uint8_t textures = 0;
    bool overlay = false;
    bool alpha_test = false;
    bool fog = false;
    // Nonzero GX fog types with (type & 7) < 4 have no exponential term.
    bool linear_fog = false;
    bool operator==(const NativeShaderVariant&) const = default;
};

struct NativeShaderSources {
    std::string glsl_vertex, glsl_fragment, cg_vertex, cg_fragment;
};

NativeShaderSources NativeShaderSource(NativeShaderVariant variant);
// Stage names identify the embedded GXP per stage: "mesh_spot2_ian", "modulate_t1_alpha_fog".
std::string NativeShaderVertexName(NativeShaderVariant variant);
std::string NativeShaderFragmentName(NativeShaderVariant variant);
