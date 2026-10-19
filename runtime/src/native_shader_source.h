#pragma once
// Fixed native shaders, written by hand for GLES and GXM (see the .cpp).
#include <cstdint>
#include <string>
#include <vector>

// Each enum's count follows it; NativeShaderVertexKeys() and
// NativeShaderFragmentKeys() iterate these ranges (docs/optimizations/rendering/offline-shader-bank.md).
// Geometry: pre-transformed vertices. Quad: compact effect quads (object
// corners and two coordinates; the draw's matrices in the Quad block).
enum class NativeShaderVertex : uint8_t {
    Mesh,
    Geometry,
    Quad
};

inline constexpr unsigned NativeShaderVertexCount = 3;
enum class NativeShaderModel : uint8_t {
    Basic,
    Modulate,
    Layers
}; // MH3_SHADE_BASIC / MH3_SHADE_MODULATE / MH3_SHADE_LAYERS

inline constexpr unsigned NativeShaderModelCount = 3;

// Mesh texgen per role, fixed at compile time. Identity and Affine read the
// role's coordinate attribute (no projection, normalize, post or palette).
// Srt is Affine followed by the role's post SRT from the Animated block
// (material uniforms only).
// Spot: colour-only spot lighting with clamped diffuse over `lights` lights;
// General: any GX channel configuration.
enum class NativeShaderLighting : uint8_t {
    None,
    Spot,
    General
};

inline constexpr unsigned NativeShaderLightingCount = 3;

// Spot lights index one of the eight GX lights per nibble.
inline constexpr unsigned NativeShaderMaxLights = 8;
enum class NativeShaderTexgen : uint8_t {
    None,
    Identity,
    Affine,
    General,
    Srt
};

inline constexpr unsigned NativeShaderTexgenCount = 5;

// Texture roles of a classified material (mh3_shade_material::textures).
inline constexpr unsigned NativeShaderMaxTextures = 3;

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

    // Exact cached single-texture RGBA product; literal zero/one coefficients.
    bool pure_modulate = false;

    // Physical target is certified A=1; preserve original alpha/discard, then
    // return A=1 so an unblended RGB mask can become a full GXM color write.
    bool target_alpha_one = false;

    // Material uniforms in GPU memory (docs/optimizations/rendering/material-uniform-blocks.md): the
    // baked Material block and the Animated block replace the per-draw Shade
    // block. dynamic: a coefficient picks an animated palette entry.
    bool material_gpu = false;
    bool dynamic = false;

    // BASIC whose colour and alpha inputs a, b and d are constants and c is
    // texture role 0 (effect particles' colour ramps): a program with only
    // that input's arithmetic per fragment.
    bool basic_lerp = false;

    // BASIC whose saturated result is multiplied by the vertex colour (a
    // closing prev * ras stage, mh3_shade_material::raster_product).
    bool raster_product = false;

    // Quad only, a basic_lerp material with material uniforms: each quad's
    // base and slope (mh3_shade_gpu_lerp) come with its vertices, so quads
    // whose colours differ draw as one (effect particles, DrawNativeQuadRun).
    bool vertex_lerp = false;

    // Mesh only: GPU skinning (docs/optimizations/rendering/gpu-skinning.md), four palette rows and
    // weights per vertex instead of the GX matrix selectors.
    bool skinned = false;
    bool operator==(const NativeShaderVariant&) const = default;
};

// A new field must reach a stage key, the key domain and the shader bank.
static_assert(sizeof(NativeShaderVariant) == 20, "update NativeShader*Key, NativeShader*Keys() and docs/optimizations/rendering/offline-shader-bank.md");

// Stage identities. Each stage's source is a function of its key alone, so
// the GXM shader bank holds one program per key (docs/optimizations/rendering/offline-shader-bank.md).
// Keys are canonical: fields their stage ignores are cleared.
struct NativeShaderVertexKey {
    NativeShaderVertex vertex = NativeShaderVertex::Mesh;
    NativeShaderLighting lighting = NativeShaderLighting::None;
    uint8_t lights = 0;                // Spot only
    NativeShaderTexgen texgen[3] = {}; // Mesh only
    bool material_gpu = false;
    bool skinned = false; // Mesh only
    bool lerp = false;    // Quad with material_gpu only: a_base and a_slope to v_base and v_slope
    auto operator<=>(const NativeShaderVertexKey&) const = default;
};

struct NativeShaderFragmentKey {
    NativeShaderModel model = NativeShaderModel::Modulate;
    uint8_t textures = 0;
    bool overlay = false;
    bool alpha_test = false;
    bool fog = false;
    bool linear_fog = false;       // only with fog
    bool pure_modulate = false;    // only Modulate, one texture, no overlay
    bool target_alpha_one = false;
    bool material_gpu = false;
    bool dynamic = false;          // only material_gpu Modulate, not pure
    bool basic_lerp = false;       // only Basic with textures, no overlay
    bool raster_product = false;   // only Basic, no overlay
    bool vertex_lerp = false;      // only basic_lerp with material_gpu
    auto operator<=>(const NativeShaderFragmentKey&) const = default;
};

// Throw std::invalid_argument outside the domain: Spot with more than
// NativeShaderMaxLights lights, more than NativeShaderMaxTextures textures, or
// an Srt role without material_gpu (its post SRT is in the Animated block).
NativeShaderVertexKey NativeShaderVertexKeyOf(const NativeShaderVariant& variant);
NativeShaderFragmentKey NativeShaderFragmentKeyOf(const NativeShaderVariant& variant);

// The variant's fragment is the pure modulate one with fog, which takes the
// fog blend folded into the vertex colour: its draws set draw.channel.w bit 17
// for the vertex program (fog_blend).
bool NativeShaderFoldsFog(const NativeShaderVariant& variant);

// The complete domain: every key of a valid variant, each once. The bank
// generator (scripts/vita_shader_bank.cpp) compiles exactly these.
std::vector<NativeShaderVertexKey> NativeShaderVertexKeys();
std::vector<NativeShaderFragmentKey> NativeShaderFragmentKeys();

struct NativeShaderSources {
    std::string glsl_vertex, glsl_fragment, cg_vertex, cg_fragment;
};

NativeShaderSources NativeShaderSource(NativeShaderVariant variant);

// The GXM (Cg) stage sources; the bank is keyed by XXH3 of these strings.
std::string NativeShaderCgVertex(const NativeShaderVertexKey& key);
std::string NativeShaderCgFragment(const NativeShaderFragmentKey& key);

#ifdef MH3_WEB_SHADER_BANK
// The GLSL ES stage sources; the web build's WGSL bank is keyed by XXH3 of
// these strings. Only its host generator compiles them.
std::string NativeShaderGlslVertex(const NativeShaderVertexKey& key);
std::string NativeShaderGlslFragment(const NativeShaderFragmentKey& key);
#endif

// Stage names identify the embedded GXP per stage: "mesh_spot2_ian", "modulate_t1_alpha_fog".
std::string NativeShaderVertexName(NativeShaderVariant variant);
std::string NativeShaderFragmentName(NativeShaderVariant variant);
std::string NativeShaderVertexName(const NativeShaderVertexKey& key);
std::string NativeShaderFragmentName(const NativeShaderFragmentKey& key);
