// Fixed native shaders, written by hand for GLES (GLSL ES 3.0) and GXM (Cg).
// Materials are classified once into these programs (runtime/native/mh3_shade.h);
// there is no TEV interpretation, per-material generation or GLSL->Cg conversion.
//
// Vertex inputs are bound per draw to fixed locations: the backend points each
// texture role's coordinate and matrix selector at the mesh's actual texcoord
// and selector byte, so the shaders never select or convert attributes.
// Selectors are already integral U8 slots. A rounding bias is unnecessary and
// GXM fastint can round biased array indices to the next matrix slot.
// Every fragment texture read uses its varying unmodified (independent reads on
// SGX); projective roles are divided per vertex and LOD bias is sampler state on
// GXM. Both use GL depth from GX clip space: (x, y, 2z - w, w). GLES also
// negates y (Aurora's aur_position) because its render targets are bottom-up;
// GXM's viewport already maps y downward, so its programs keep y (as the old
// converted programs did), which also keeps the guest's winding for culling.
// Fog eye depth is linear in eye space, so it is computed per vertex and
// interpolated exactly; the fragment only applies the curve.
//
// Blocks: Draw (vertex, binding 0 / vertex BUFFER[0]), Scene (both, binding 1 /
// BUFFER[1]), Palette (mesh vertex, binding 2 / vertex BUFFER[2]), Shade
// (fragment, binding 3 / fragment BUFFER[0]) and View (mesh vertex, binding 6 /
// vertex BUFFER[4]: the projection and viewport, shared by the draws). Draw and Shade share one per-draw
// allocation (mh3_shade_draw); Shade's tail differs per fragment model.
// MATERIAL_GPU programs (docs/optimizations/rendering/material-uniform-blocks.md) replace Shade with the
// baked Material block (binding 4 / fragment BUFFER[2]) and the Animated
// block (binding 5 / fragment BUFFER[3], mesh vertex BUFFER[3]); coefficients
// are static * picked animated palette entries, evaluated from uniforms only.
#include "native_shader_source.h"
#include "fatal.h"
#include "../native/mh3_shade_chain.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>

namespace {
// ---------------------------------------------------------------- GLSL ES 3.0
constexpr const char* GlslPrelude = R"GLSL(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
)GLSL";

// std140 layouts mirror mh3_shade_scene / mh3_shade_vertex / mh3_shade_fragment.
constexpr const char* GlslScene = R"GLSL(
struct Light {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 angular;
    vec4 distance;
};
layout(std140) uniform Scene {
    Light lights[8];
    vec4 fog_color;
    vec4 fog;
    vec4 fog_curve;
    vec4 fog_shape;
} scene;
)GLSL";

constexpr const char* GlslDraw = R"GLSL(
struct Texgen {
    mat3x4 main;
    mat3x4 post;
    ivec4 mode;
};
layout(std140) uniform Draw {
    ivec4 flags;
    vec4 channel_material;
    vec4 channel_ambient;
    ivec4 channel;
    Texgen texgen[3];
} draw;
)GLSL";

constexpr const char* GlslShade = R"GLSL(
struct Input {
    vec4 offset;
    vec4 scale;
    vec4 select;
    vec4 swizzle;
};
#if MATERIAL_GPU
// mh3_shade_gpu_material
struct Pick {
    vec4 index;
    vec4 color_mask;
    vec4 alpha_mask;
};
layout(std140) uniform Material {
    vec4 coefficient[9];
    Pick pick[9];
    vec4 add;
    vec4 scale;
    vec4 equation[2];
    vec4 select[8];
    vec4 swizzle;
} material;
#else
layout(std140) uniform Shade {
    vec4 texture_bias;
    vec4 overlay;
    vec4 alpha_range;
#if MODULATE
    vec4 w0;
    vec4 w1;
    vec4 offset;
    vec4 kv;
    vec4 kc;
    vec4 d;
    vec4 s;
#elif LAYERS
    vec4 kv[4];
    vec4 kc[4];
    vec4 s;
    vec4 mv;
    vec4 mc;
#else
    Input color_input[4];
    Input alpha_input[4];
    vec4 color_equation;
    vec4 alpha_equation;
#endif
} shade;
#endif
)GLSL";

// mh3_shade_gpu_animated: fragment colours/alpha/bias, mesh vertex SRT.
constexpr const char* GlslAnimated = R"GLSL(
#if MATERIAL_GPU
struct Srt {
    vec4 scale_rotation;
    vec4 translation;
};
layout(std140) uniform Animated {
    vec4 palette[9];
    vec4 alpha_range;
    vec4 texture_bias;
    Srt srt[3];
} anim;
// Post SRT of a role (mh3_shade_gpu_srt): the XSI matrix of 80075F00.
vec2 srt2d(int role, vec2 uv) {
    vec4 p = anim.srt[role].scale_rotation;
    vec2 t = anim.srt[role].translation.xy;
    float a = p.x * p.w, b = p.x * p.z, c = p.y * p.z, d = p.y * p.w;
    return vec2(a * uv.x - b * uv.y + ((b - a * t.x) - b * t.y), c * uv.x + d * uv.y + ((-d - c * t.x) + d * t.y + 1.0));
}
#endif
)GLSL";

// channel: x colour light mask (LIT 2) or packed light indices (LIT 1), y alpha
// light mask, z bits (1 colour lit, 2 alpha lit, 4/16 material from vertex,
// 8/32 ambient from vertex), w light functions (colour diffuse | attenuation
// << 2, alpha in bits 8..11; bit 16: LIT 1's lights are all far). LIT 1 is
// colour-only spot lighting over LIGHTS lights; LIT 2 evaluates any GX channel.
constexpr const char* GlslVertexCommon = R"GLSL(
#if LIT == 1
// Spot attenuation with clamped diffuse, the only colour lighting MH3 models use.
vec3 spot_light(int i, vec3 position, vec3 normal) {
    Light light = scene.lights[i];
    vec3 ldir = light.position.xyz - position;
    float dist2 = dot(ldir, ldir);
    float inverse = inversesqrt(max(dist2, 1e-30));
    float dist = dist2 * inverse;
    ldir *= inverse;
    float cosine = max(0.0, dot(ldir, light.direction.xyz));
    float attn = max(0.0, dot(light.angular.xyz, vec3(1.0, cosine, cosine * cosine)) / dot(light.distance.xyz, vec3(1.0, dist, dist2)));
    return attn * max(0.0, dot(ldir, normal)) * light.color.rgb;
}

// A far light (channel.w bit 16: every light of the draw has neither angle nor
// distance attenuation and lies at least 1e6 units from the eye, the GX form
// of a directional light): its direction from the eye stands for the one from
// the vertex.
vec3 far_light(int i, vec3 normal) {
    Light light = scene.lights[i];
    return max(0.0, dot(normalize(light.position.xyz), normal)) * light.color.rgb;
}
#elif LIT == 2
float light_term(int i, vec3 position, vec3 normal, int functions) {
    Light light = scene.lights[i];
    int diffuse = functions & 3, attenuation = (functions >> 2) & 3;
    vec3 ldir = light.position.xyz - position;
    float dist2 = dot(ldir, ldir);
    float dist = sqrt(dist2);
    ldir = dist > 0.0 ? ldir / dist : normal;
    float attn = 1.0;
    if (attenuation == 1) {
        float cosine = max(0.0, dot(ldir, light.direction.xyz));
        float cos_attn = dot(light.angular.xyz, vec3(1.0, cosine, cosine * cosine));
        float dist_attn = dot(light.distance.xyz, vec3(1.0, dist, dist2));
        attn = max(0.0, cos_attn / dist_attn);
    } else if (attenuation == 0) {
        attn = dot(normal, ldir) >= 0.0 ? max(0.0, dot(normal, light.direction.xyz)) : 0.0;
        float cos_attn = dot(light.angular.xyz, vec3(1.0, attn, attn * attn));
        vec3 coefficients = diffuse != 0 ? normalize(light.distance.xyz) : light.distance.xyz;
        float dist_attn = dot(coefficients, vec3(1.0, attn, attn * attn));
        attn = dist_attn != 0.0 ? max(0.0, cos_attn / dist_attn) : (cos_attn > 0.0 ? 1.0 : 0.0);
    }
    float diff = diffuse == 0 ? 1.0 : diffuse == 1 ? dot(ldir, normal) : max(0.0, dot(ldir, normal));
    return attn * diff;
}
#endif
vec4 light_channel(vec3 position, vec3 normal, vec4 vertex) {
    int bits = draw.channel.z;
    vec3 material = (bits & 4) != 0 ? vertex.rgb : draw.channel_material.rgb;
    float material_alpha = (bits & 16) != 0 ? vertex.a : draw.channel_material.a;
    vec4 result = vec4(material, material_alpha);
#if LIT == 1
    // channel.x packs the active light indices, one per nibble.
    vec3 ambient = (bits & 8) != 0 ? vertex.rgb : draw.channel_ambient.rgb;
    if ((draw.channel.w & 0x10000) != 0) {
        for (int n = 0; n < LIGHTS; ++n)
            ambient += far_light((draw.channel.x >> (4 * n)) & 7, normal);
    } else {
        for (int n = 0; n < LIGHTS; ++n)
            ambient += spot_light((draw.channel.x >> (4 * n)) & 7, position, normal);
    }
    result.rgb = material * clamp(ambient, 0.0, 1.0);
#elif LIT == 2
    if ((bits & 1) != 0) {
        vec3 accumulated = (bits & 8) != 0 ? vertex.rgb : draw.channel_ambient.rgb;
        for (int i = 0; i < 8; ++i)
            if ((draw.channel.x & (1 << i)) != 0)
                accumulated += light_term(i, position, normal, draw.channel.w & 15) * scene.lights[i].color.rgb;
        result.rgb = material * clamp(accumulated, 0.0, 1.0);
    }
    if ((bits & 2) != 0) {
        float accumulated = (bits & 32) != 0 ? vertex.a : draw.channel_ambient.a;
        for (int i = 0; i < 8; ++i)
            if ((draw.channel.y & (1 << i)) != 0)
                accumulated += light_term(i, position, normal, (draw.channel.w >> 8) & 15) * scene.lights[i].color.a;
        result.a = material_alpha * clamp(accumulated, 0.0, 1.0);
    }
#endif
    return result;
}
// clip uses the GX convention: z in [0, w], reversed depth.
float fog_distance(vec4 clip) {
    float depth = 1.0 - clip.z / clip.w;
    float ze = scene.fog_shape.z != 0.0 ? scene.fog.x * depth : scene.fog.x / (scene.fog.y - depth * scene.fog.w);
    return ze - scene.fog.z;
}

// The fog amount at a vertex (the curve of its clamped distance; a linear
// curve is the distance itself), which the fragment blends by as
// interpolated. Exact for linear fog wherever a triangle's corners are all
// inside the fog range; across its ends the clamp is taken at the corners.
float fog_amount(vec4 clip) {
    float f = clamp(fog_distance(clip), 0.0, 1.0);
    float g = scene.fog_curve.x + scene.fog_curve.y * f;
    g = mix(g, g * g, scene.fog_curve.z);
    return clamp(scene.fog_curve.w * f + scene.fog_shape.x + scene.fog_shape.y * exp2(-8.0 * g), 0.0, 1.0);
}

// The fog blend as the fragment takes it (v_fog): f times the fog colour and
// 1 - f, one multiply-add there. For the pure modulate fragment
// (draw.channel.w bit 17, kFoldFog in native_material.cpp) 1 - f goes into the
// vertex colour instead and w is zero, so that fragment is texel * colour +
// fog. That is approximate: the triangle interpolates the colour's product
// with 1 - f instead of multiplying the interpolated values.
vec4 fog_blend(vec4 clip, inout vec4 color) {
    float f = fog_amount(clip);
    vec4 blend = vec4(f * scene.fog_color.rgb, 1.0 - f);

    if ((draw.channel.w & 0x20000) != 0) {
        color.rgb *= blend.w;
        blend.w = 0.0;
    }

    return blend;
}
)GLSL";

constexpr const char* GlslMeshVertex = R"GLSL(
// mh3_shade_view: shared by the draws of one projection and viewport.
layout(std140) uniform View {
    mat4 proj;
    vec4 viewport;
} view;
#if SKINNED
// The mesh's palette rows (docs/optimizations/rendering/gpu-skinning.md): three vec4 per matrix.
layout(std140) uniform Palette {
    vec4 rows[765];
} palette;
layout(location=10) in vec4 a_bones;
layout(location=11) in vec4 a_weights;
#else
layout(std140) uniform Palette {
    mat3x4 position[20];
    mat3x4 normal[10];
} palette;
#endif
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec4 a_color0;
layout(location=3) in vec2 a_uv0;
layout(location=4) in vec2 a_uv1;
layout(location=5) in vec2 a_uv2;
layout(location=6) in float a_select;
layout(location=7) in float a_select0;
layout(location=8) in float a_select1;
layout(location=9) in float a_select2;
out vec4 v_color0;
out vec2 v_uv0;
out vec2 v_uv1;
out vec2 v_uv2;
out vec4 v_fog;
// mode.x source: 0 model position, 1 model normal, 2 role coordinate attribute.
// mode.y bits: 1 projective (three-row result), 2 normalize, 4 main identity,
// 8 post identity, 16 main from the palette slot in the role selector, 32 post
// from the role's Animated SRT (MATERIAL_GPU).
#if SKINNED
// m: the vertex's blended matrix rows; n: its normal (environment) matrix rows.
vec2 texgen(int role, vec2 uv, vec4 m0, vec4 m1, vec4 m2, vec3 n0, vec3 n1, vec3 n2, vec3 position, vec3 normal) {
#else
vec2 texgen(int role, vec2 uv, float selector, vec3 position, vec3 normal) {
#endif
    Texgen t = draw.texgen[role];
    int bits = t.mode.y;
    vec4 source = t.mode.x == 0 ? vec4(position, 1.0) : t.mode.x == 1 ? vec4(normal, 1.0) : vec4(uv, 1.0, 1.0);
    vec3 coordinate = source.xyz;
    if ((bits & 16) != 0) {
#if SKINNED
        // 64: the environment matrix (the normal matrix, no translation).
        coordinate = (bits & 64) != 0 ? vec3(dot(source.xyz, n0), dot(source.xyz, n1), dot(source.xyz, n2)) : vec3(dot(source, m0), dot(source, m1), dot(source, m2));
#else
        int slot = int(selector);
        if (slot < 20)
            coordinate = source * palette.position[slot];
#endif
    } else if ((bits & 4) == 0)
        coordinate = source * t.main;
    if ((bits & 1) == 0)
        coordinate.z = 1.0;
    if ((bits & 2) != 0)
        coordinate = normalize(coordinate);
#if MATERIAL_GPU
    // 32: the post matrix is the role's SRT (row 2 keeps z).
    if ((bits & 32) != 0)
        coordinate.xy = srt2d(role, coordinate.xy);
    else
#endif
    if ((bits & 8) == 0)
        coordinate = vec4(coordinate, 1.0) * t.post;
    return coordinate.z != 0.0 ? coordinate.xy / coordinate.z : clamp(coordinate.xy / 2.0, -1.0, 1.0);
}
vec2 affine(int role, vec2 uv) { return (vec4(uv, 1.0, 1.0) * draw.texgen[role].main).xy; }
void main() {
#if SKINNED
    // M = sum weight * row matrix; its normal matrix is the inverse-transpose
    // (cofactor / det), the plain 3x3 when singular, as the game's view pass.
    ivec4 b = ivec4(a_bones) * 3;
    vec4 w = a_weights;
    vec4 m0 = w.x * palette.rows[b.x] + w.y * palette.rows[b.y] + w.z * palette.rows[b.z] + w.w * palette.rows[b.w];
    vec4 m1 = w.x * palette.rows[b.x + 1] + w.y * palette.rows[b.y + 1] + w.z * palette.rows[b.z + 1] + w.w * palette.rows[b.w + 1];
    vec4 m2 = w.x * palette.rows[b.x + 2] + w.y * palette.rows[b.y + 2] + w.z * palette.rows[b.z + 2] + w.w * palette.rows[b.w + 2];
    vec3 n0 = cross(m1.xyz, m2.xyz), n1 = cross(m2.xyz, m0.xyz), n2 = cross(m0.xyz, m1.xyz);
    float det = dot(m0.xyz, n0);
    if (abs(det) < 1e-36) {
        n0 = m0.xyz;
        n1 = m1.xyz;
        n2 = m2.xyz;
    } else {
        n0 /= det;
        n1 /= det;
        n2 /= det;
    }
    vec4 p = vec4(a_position, 1.0);
    vec3 position = vec3(dot(p, m0), dot(p, m1), dot(p, m2));
#else
    int slot = (draw.flags.x & 1) != 0 ? int(a_select) : 0;
    vec3 position = vec4(a_position, 1.0) * palette.position[slot];
#endif
    vec4 projected = vec4(position, 1.0) * view.proj;
    vec4 vertex = (draw.flags.x & 2) != 0 ? a_color0 : vec4(1.0);
#if LIT
#if SKINNED
    vec3 normal = vec3(dot(a_normal, n0), dot(a_normal, n1), dot(a_normal, n2));
#else
    vec3 normal = vec4(a_normal, 0.0) * palette.normal[slot];
#endif
    normal = dot(normal, normal) > 1e-10 ? normalize(normal) : normal;
    v_color0 = light_channel(position, normal, vertex);
#else
    v_color0 = light_channel(position, vec3(0.0), vertex);
#endif
    v_fog = fog_blend(projected, v_color0);
    projected.xy += projected.w * view.viewport.zw;
    TEXGEN_OUTPUTS
    gl_Position = vec4(projected.x, -projected.y, 2.0 * projected.z - projected.w, projected.w);
}
)GLSL";

// Compact effect quads (AuroraNativeQuadVertex): mh3_effect_material_vertices'
// arithmetic per vertex. The Quad block takes the palette's place: the draw's
// position matrix and projection, the normal and colour every quad of the draw
// shares, and per role the coordinate it reads (0, 1, or 2: none, zero); w is
// 1 for a reversed depth range.
constexpr const char* GlslQuadVertex = R"GLSL(
layout(std140) uniform Palette {
    mat3x4 position;
    mat4 proj;
    vec4 normal;
    vec4 color0;
    ivec4 mode;
} quad;
layout(location=0) in vec3 a_position;
layout(location=1) in vec3 a_uv0;
layout(location=2) in vec3 a_uv1;
out vec4 v_color0;
out vec2 v_uv0;
out vec2 v_uv1;
out vec2 v_uv2;
out vec4 v_fog;
#ifdef LERP
// The quad's BASIC_LERP base and slope (mh3_shade_gpu_lerp), the same at its corners.
layout(location=3) in vec4 a_base;
layout(location=4) in vec4 a_slope;
out vec4 v_base;
out vec4 v_slope;
#endif
vec2 affine(vec3 uv) { return uv.z != 0.0 ? uv.xy / uv.z : uv.xy; }
vec2 coordinate(int source) { return source == 0 ? affine(a_uv0) : source == 1 ? affine(a_uv1) : vec2(0.0); }
void main() {
    vec3 view = vec4(a_position, 1.0) * quad.position;
    vec4 clip = vec4(view, 1.0) * quad.proj;
    float depth = quad.mode.w != 0 ? -clip.z : clip.z + clip.w;
    vec4 p = vec4(clip.xy, 2.0 * depth - clip.w, clip.w);
    v_color0 = light_channel(view, quad.normal.xyz, quad.color0);
    v_fog = fog_blend(vec4(p.xy, 0.5 * (p.z + p.w), p.w), v_color0);
    v_uv0 = coordinate(quad.mode.x);
    v_uv1 = coordinate(quad.mode.y);
    v_uv2 = coordinate(quad.mode.z);
#ifdef LERP
    v_base = a_base;
    v_slope = a_slope;
#endif
    gl_Position = vec4(p.x, -p.y, p.z, p.w);
}
)GLSL";

// Pre-transformed application geometry (UI, effects, fades, feedback). The
// producer already generated each coordinate; roles bind them to a_uv0..2.
constexpr const char* GlslGeometryVertex = R"GLSL(
layout(location=0) in vec4 a_position;
layout(location=1) in vec3 a_view_position;
layout(location=2) in vec3 a_normal;
layout(location=3) in vec4 a_color0;
layout(location=4) in vec3 a_uv0;
layout(location=5) in vec3 a_uv1;
layout(location=6) in vec3 a_uv2;
out vec4 v_color0;
out vec2 v_uv0;
out vec2 v_uv1;
out vec2 v_uv2;
out vec4 v_fog;
vec2 affine(vec3 uv) { return uv.z != 0.0 ? uv.xy / uv.z : uv.xy; }
void main() {
    v_color0 = light_channel(a_view_position, a_normal, a_color0);
    // a_position is GL clip space (z in [-w, w]); fog reads the GX depth.
    vec4 p = a_position;
    v_fog = fog_blend(vec4(p.xy, 0.5 * (p.z + p.w), p.w), v_color0);
    v_uv0 = affine(a_uv0);
    v_uv1 = affine(a_uv1);
    v_uv2 = affine(a_uv2);
    gl_Position = vec4(p.x, -p.y, p.z, p.w);
}
)GLSL";

// MODULATE: out = ((t0 * w0 + t1 * w1 + offset [+ overlay * t1 * t2]) * (v * kv + kc) + d) * s.
// BASIC: out = (d + bias + sign * mix(a, b, c)) * scale for colour and alpha;
// each input is offset + scale * (select . (t0, t1, t2, vertex)), swizzle.x
// replicating the source's alpha into a colour input.
// OVERLAY adds overlay.rgb * texture1 * texture2 to the colour base / b input.
// LAYERS: out = saturate(saturate((sum_r (v * kv[r] + kc[r]) * t_r + v * kv[3] +
// kc[3]) * s) * (v * mv + mc)): textures with their own vertex factors summed.
constexpr const char* GlslFragment = R"GLSL(
in vec4 v_color0;
in vec2 v_uv0;
in vec2 v_uv1;
in vec2 v_uv2;
in vec4 v_fog;
#ifdef VERTEX_LERP
in vec4 v_base;
in vec4 v_slope;
#endif
uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_texture2;
out vec4 frag_color;
#if MATERIAL_GPU
// Uniform-only: static factor times the picked animated palette entry.
vec4 coefficient(int k) {
#if DYNAMIC
    Pick p = material.pick[k];
    vec4 c = anim.palette[int(p.index.x)];
    vec4 a = anim.palette[int(p.index.y)];
    return material.coefficient[k] * vec4(mix(c.rgb, vec3(dot(c, p.color_mask)), p.index.z), dot(a, p.alpha_mask));
#else
    return material.coefficient[k];
#endif
}
#define TEXTURE_BIAS anim.texture_bias
#define ALPHA_RANGE anim.alpha_range
#else
#define TEXTURE_BIAS shade.texture_bias
#define ALPHA_RANGE shade.alpha_range
#endif
#if !MODULATE && !LAYERS
vec4 source(vec4 select, vec4 t0, vec4 t1, vec4 t2) {
    return select.x * t0 + select.y * t1 + select.z * t2 + select.w * v_color0;
}
#if MATERIAL_GPU
vec3 color_input(int i, float swizzle, vec4 t0, vec4 t1, vec4 t2) {
    vec4 value = source(material.select[i], t0, t1, t2);
    return coefficient(i).rgb + coefficient(4 + i).rgb * mix(value.rgb, value.aaa, swizzle);
}
float alpha_input(int i, vec4 t0, vec4 t1, vec4 t2) {
    return coefficient(i).a + coefficient(4 + i).a * source(material.select[4 + i], t0, t1, t2).a;
}
#define SWIZZLE0 material.swizzle.x
#define SWIZZLE1 material.swizzle.y
#define SWIZZLE2 material.swizzle.z
#define SWIZZLE3 material.swizzle.w
#define COLOR_EQUATION material.equation[0]
#define ALPHA_EQUATION material.equation[1]
#define OVERLAY_SCALE coefficient(8)
#define LERP_COLOR_OFFSET(i) coefficient(i)
#define LERP_ALPHA_OFFSET(i) coefficient(i).a
#define LERP_COLOR_SCALE coefficient(6)
#define LERP_ALPHA_SCALE coefficient(6).a
#define LERP_SWIZZLE material.swizzle.z
#else
vec3 color_input(int i, float swizzle, vec4 t0, vec4 t1, vec4 t2) {
    Input s = shade.color_input[i];
    vec4 value = source(s.select, t0, t1, t2);
    return s.offset.rgb + s.scale.rgb * mix(value.rgb, value.aaa, s.swizzle.x);
}
float alpha_input(int i, vec4 t0, vec4 t1, vec4 t2) {
    Input s = shade.alpha_input[i];
    return s.offset.a + s.scale.a * source(s.select, t0, t1, t2).a;
}
#define SWIZZLE0 0.0
#define SWIZZLE1 0.0
#define SWIZZLE2 0.0
#define SWIZZLE3 0.0
#define COLOR_EQUATION shade.color_equation
#define ALPHA_EQUATION shade.alpha_equation
#define OVERLAY_SCALE shade.overlay
#define LERP_COLOR_OFFSET(i) shade.color_input[i].offset
#define LERP_ALPHA_OFFSET(i) shade.alpha_input[i].offset.a
#define LERP_COLOR_SCALE shade.color_input[2].scale
#define LERP_ALPHA_SCALE shade.alpha_input[2].scale.a
#define LERP_SWIZZLE shade.color_input[2].swizzle.x
#endif
#endif
void main() {
    vec4 t0 = vec4(1.0), t1 = vec4(1.0), t2 = vec4(1.0);
#if TEXTURES > 0
    t0 = texture(u_texture0, v_uv0, TEXTURE_BIAS.x);
#endif
#if TEXTURES > 1
    t1 = texture(u_texture1, v_uv1, TEXTURE_BIAS.y);
#endif
#if TEXTURES > 2
    t2 = texture(u_texture2, v_uv2, TEXTURE_BIAS.z);
#endif
#if MODULATE && PURE_MODULATE && FOG
    // The vertices folded the fog blend into the colour (fog_blend).
    vec4 result = t0 * v_color0 + v_fog;
#elif MODULATE && PURE_MODULATE
    vec4 result;
    result.rg = clamp(t0.rg * v_color0.rg, 0.0, 1.0);
    result.ba = clamp(t0.ba * v_color0.ba, 0.0, 1.0);
#elif MODULATE
#if MATERIAL_GPU
    vec4 w0 = coefficient(0), w1 = coefficient(1), offset = coefficient(2), kv = coefficient(3), kc = coefficient(4);
    vec4 d = coefficient(5) + material.add, s = material.scale, overlay = coefficient(6);
#else
    vec4 w0 = shade.w0, w1 = shade.w1, offset = shade.offset, kv = shade.kv, kc = shade.kc, d = shade.d, s = shade.s, overlay = shade.overlay;
#endif
    vec4 base = t0 * w0 + offset;
#if TEXTURES == 2
    base += t1 * w1;
#endif
#if OVERLAY
    base.rgb += overlay.rgb * t1.rgb * t2.rgb;
#endif
    vec4 result = clamp((base * (v_color0 * kv + kc) + d) * s, 0.0, 1.0);
#elif LAYERS
    vec4 v = v_color0;
    vec4 sum = (v * shade.kv[0] + shade.kc[0]) * t0 + v * shade.kv[3] + shade.kc[3];
#if TEXTURES > 1
    sum += (v * shade.kv[1] + shade.kc[1]) * t1;
#endif
#if TEXTURES > 2
    sum += (v * shade.kv[2] + shade.kc[2]) * t2;
#endif
    vec4 result = clamp(clamp(sum * shade.s, 0.0, 1.0) * (v * shade.mv + shade.mc), 0.0, 1.0);
#elif BASIC_LERP && defined(VERTEX_LERP)
    // The quad's base and slope came with its vertices.
    vec4 result = clamp(v_base + v_slope * vec4(mix(t0.rgb, t0.aaa, LERP_SWIZZLE), t0.a), 0.0, 1.0);
#elif BASIC_LERP
    // Constant inputs are their offsets; only c (texture role 0) varies, so
    // the whole equation is affine in it: base + slope * texel, both from
    // uniforms only (GXM's secondary program evaluates them once per draw).
    vec3 range = LERP_COLOR_OFFSET(1).rgb - LERP_COLOR_OFFSET(0).rgb;
    vec3 slope = range * LERP_COLOR_SCALE.rgb * (COLOR_EQUATION.z * COLOR_EQUATION.y);
    vec3 base = (LERP_COLOR_OFFSET(3).rgb + COLOR_EQUATION.x + COLOR_EQUATION.z * (LERP_COLOR_OFFSET(0).rgb + range * LERP_COLOR_OFFSET(2).rgb)) * COLOR_EQUATION.y;
    float alpha_range = LERP_ALPHA_OFFSET(1) - LERP_ALPHA_OFFSET(0);
    float alpha_slope = alpha_range * LERP_ALPHA_SCALE * (ALPHA_EQUATION.z * ALPHA_EQUATION.y);
    float alpha_base = (LERP_ALPHA_OFFSET(3) + ALPHA_EQUATION.x + ALPHA_EQUATION.z * (LERP_ALPHA_OFFSET(0) + alpha_range * LERP_ALPHA_OFFSET(2))) * ALPHA_EQUATION.y;
    vec4 result = clamp(vec4(base, alpha_base) + vec4(slope, alpha_slope) * vec4(mix(t0.rgb, t0.aaa, LERP_SWIZZLE), t0.a), 0.0, 1.0);
#else
    vec3 b = color_input(1, SWIZZLE1, t0, t1, t2);
#if OVERLAY
    b += OVERLAY_SCALE.rgb * t1.rgb * t2.rgb;
#endif
    vec3 color = mix(color_input(0, SWIZZLE0, t0, t1, t2), b, color_input(2, SWIZZLE2, t0, t1, t2));
    color = (color_input(3, SWIZZLE3, t0, t1, t2) + COLOR_EQUATION.x + COLOR_EQUATION.z * color) * COLOR_EQUATION.y;
    float alpha = mix(alpha_input(0, t0, t1, t2), alpha_input(1, t0, t1, t2), alpha_input(2, t0, t1, t2));
    alpha = (alpha_input(3, t0, t1, t2) + ALPHA_EQUATION.x + ALPHA_EQUATION.z * alpha) * ALPHA_EQUATION.y;
    vec4 result = clamp(vec4(color, alpha), 0.0, 1.0);
#endif
#if RASTER_PRODUCT
    result *= v_color0;
#endif
#if ALPHA_TEST
    if (result.a < ALPHA_RANGE.x || result.a >= ALPHA_RANGE.y)
        discard;
#endif
#if FOG && !(MODULATE && PURE_MODULATE)
    // f times the fog colour and 1 - f, as the vertices computed them (fog_blend).
    result.rgb = result.rgb * v_fog.w + v_fog.rgb;
#endif
#if TARGET_ALPHA_ONE
    result.a = 1.0;
#endif
    frag_color = result;
}
)GLSL";

// CHAIN (mh3_shade.h): x0 = x = saturate(t0 * (v * base_v + base_c) +
// base_d), then per step x = mix(x, saturate((x * (v * keep_v + keep_c) + x0 *
// keep_base + L * (v * layer_v + layer_c)) * s), M * mask_scale +
// mask_offset) with the form's layer and mask roles L and M (one without; a
// mask its role's colour or alpha), on the colour; alpha =
// saturate(saturate(sum) * times * (v.a * alpha_factor.x + alpha_factor.y)),
// sum and times selects over the roles' alphas, v.a and one. Roles 3 and 4
// sample at role 0's coordinate. Its own program text: the other models'
// programs stay as they were.
constexpr const char* GlslChainFragment = R"GLSL(
struct Step {
    vec4 keep_v;
    vec4 keep_c;
    vec4 keep_base;
    vec4 layer_v;
    vec4 layer_c;
    vec4 mask_scale;
    vec4 mask_offset;
    vec4 s;
};
layout(std140) uniform Shade {
    vec4 texture_bias;
    vec4 overlay;
    vec4 alpha_range;
    vec4 base_v;
    vec4 base_c;
    vec4 base_d;
    Step step[3];
    vec4 alpha_sum;
    vec4 alpha_sum2;
    vec4 alpha_times;
    vec4 alpha_times2;
    vec4 alpha_factor;
    vec4 mask_bias;
} shade;
in vec4 v_color0;
in vec2 v_uv0;
in vec2 v_uv1;
in vec2 v_uv2;
in vec4 v_fog;
uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_texture2;
uniform sampler2D u_texture3;
uniform sampler2D u_texture4;
out vec4 frag_color;
// base: x0 * keep_base for a step that reads the base (BASE bit i), else zero.
vec3 chain_step(int i, vec3 x, vec3 base, vec3 v, vec3 layer, vec3 mask) {
    Step k = shade.step[i];
    vec3 p = clamp((x * (v * k.keep_v.rgb + k.keep_c.rgb) + base + layer * (v * k.layer_v.rgb + k.layer_c.rgb)) * k.s.rgb, 0.0, 1.0);
    return mix(x, p, mask * k.mask_scale.rgb + k.mask_offset.rgb);
}
void main() {
    vec4 one = vec4(1.0), t1 = one, t2 = one, t3 = one, t4 = one;
    vec4 t0 = texture(u_texture0, v_uv0, shade.texture_bias.x);
#if ROLES & 2
    t1 = texture(u_texture1, v_uv1, shade.texture_bias.y);
#endif
#if ROLES & 4
    t2 = texture(u_texture2, v_uv2, shade.texture_bias.z);
#endif
#if ROLES & 8
    t3 = texture(u_texture3, v_uv0, shade.mask_bias.x);
#endif
#if ROLES & 16
    t4 = texture(u_texture4, v_uv0, shade.mask_bias.y);
#endif
    vec4 v = v_color0;
    vec3 x = clamp(t0.rgb * (v.rgb * shade.base_v.rgb + shade.base_c.rgb) + shade.base_d.rgb, 0.0, 1.0);
    vec3 x0 = x, none = vec3(0.0);
#if STEPS > 0
    x = chain_step(0, x, (BASE & 1) != 0 ? x0 * shade.step[0].keep_base.rgb : none, v.rgb, LAYER0.rgb, MASK0.rgb);
#endif
#if STEPS > 1
    x = chain_step(1, x, (BASE & 2) != 0 ? x0 * shade.step[1].keep_base.rgb : none, v.rgb, LAYER1.rgb, MASK1.rgb);
#endif
#if STEPS > 2
    x = chain_step(2, x, (BASE & 4) != 0 ? x0 * shade.step[2].keep_base.rgb : none, v.rgb, LAYER2.rgb, MASK2.rgb);
#endif
    vec4 alphas = vec4(t0.a, t1.a, t2.a, t3.a);
    vec3 others = vec3(t4.a, v.a, 1.0);
    float a = dot(shade.alpha_sum, alphas) + dot(shade.alpha_sum2.xyz, others);
#if ALPHA_SUM
    a = clamp(a, 0.0, 1.0) * (dot(shade.alpha_times, alphas) + dot(shade.alpha_times2.xyz, others));
#endif
    vec4 result = vec4(x, clamp(a * (v.a * shade.alpha_factor.x + shade.alpha_factor.y), 0.0, 1.0));
#if ALPHA_TEST
    if (result.a < shade.alpha_range.x || result.a >= shade.alpha_range.y)
        discard;
#endif
#if FOG
    result.rgb = result.rgb * v_fog.w + v_fog.rgb;
#endif
#if TARGET_ALPHA_ONE
    result.a = 1.0;
#endif
    frag_color = result;
}
)GLSL";

// ------------------------------------------------------------------- Cg (GXM)
// Matrices are stored as three row vectors (GX 3x4); float4x3 column-major makes
// mul(v, M) match GLSL's v * mat3x4.
constexpr const char* CgScene = R"CG(#pragma pack_matrix(column_major)
struct Light {
    float4 position;
    float4 direction;
    float4 color;
    float4 angular;
    float4 distance;
};
struct Scene {
    Light lights[8];
    float4 fog_color;
    float4 fog;
    float4 fog_curve;
    float4 fog_shape;
};
uniform Scene scene : BUFFER[1];
)CG";

constexpr const char* CgDraw = R"CG(
struct Texgen {
    float4x3 main;
    float4x3 post;
    int4 mode;
};
struct Draw {
    int4 flags;
    float4 channel_material;
    float4 channel_ambient;
    int4 channel;
    Texgen texgen[3];
};
uniform Draw draw : BUFFER[0];
)CG";

constexpr const char* CgShade = R"CG(
struct Input {
    float4 offset;
    float4 scale;
    float4 select;
    float4 swizzle;
};
#if MATERIAL_GPU
struct Pick {
    float4 index;
    float4 color_mask;
    float4 alpha_mask;
};
struct Material {
    float4 coefficient[9];
    Pick pick[9];
    float4 add;
    float4 scale;
    float4 equation[2];
    float4 select[8];
    float4 swizzle;
};
uniform Material material : BUFFER[2];
#else
struct Shade {
    float4 texture_bias;
    float4 overlay;
    float4 alpha_range;
#if MODULATE
    float4 w0;
    float4 w1;
    float4 offset;
    float4 kv;
    float4 kc;
    float4 d;
    float4 s;
#elif LAYERS
    float4 kv[4];
    float4 kc[4];
    float4 s;
    float4 mv;
    float4 mc;
#else
    Input color_input[4];
    Input alpha_input[4];
    float4 color_equation;
    float4 alpha_equation;
#endif
};
uniform Shade shade : BUFFER[0];
#endif
)CG";

constexpr const char* CgAnimated = R"CG(
#if MATERIAL_GPU
struct Srt {
    float4 scale_rotation;
    float4 translation;
};
struct Animated {
    float4 palette[9];
    float4 alpha_range;
    float4 texture_bias;
    Srt srt[3];
};
uniform Animated anim : BUFFER[3];
float2 srt2d(int role, float2 uv) {
    float4 p = anim.srt[role].scale_rotation;
    float2 t = anim.srt[role].translation.xy;
    float a = p.x * p.w, b = p.x * p.z, c = p.y * p.z, d = p.y * p.w;
    return float2(a * uv.x - b * uv.y + ((b - a * t.x) - b * t.y), c * uv.x + d * uv.y + ((-d - c * t.x) + d * t.y + 1.0f));
}
#endif
)CG";

constexpr const char* CgVertexCommon = R"CG(
#if LIT == 1
float3 spot_light(int i, float3 position, float3 normal) {
    Light light = scene.lights[i];
    float3 ldir = light.position.xyz - position;
    float dist2 = dot(ldir, ldir);
    float inverse = rsqrt(max(dist2, 1e-30f));
    float dist = dist2 * inverse;
    ldir *= inverse;
    float cosine = max(0.0f, dot(ldir, light.direction.xyz));
    float attn = max(0.0f, dot(light.angular.xyz, float3(1.0f, cosine, cosine * cosine)) / dot(light.distance.xyz, float3(1.0f, dist, dist2)));
    return attn * max(0.0f, dot(ldir, normal)) * light.color.rgb;
}
// A far light (channel.w bit 16, as the GLSL): a quarter of spot_light's cost.
float3 far_light(int i, float3 normal) {
    Light light = scene.lights[i];
    return max(0.0f, dot(normalize(light.position.xyz), normal)) * light.color.rgb;
}
#elif LIT == 2
float light_term(int i, float3 position, float3 normal, int functions) {
    Light light = scene.lights[i];
    int diffuse = functions & 3;
    int attenuation = (functions >> 2) & 3;
    float3 ldir = light.position.xyz - position;
    float dist2 = dot(ldir, ldir);
    float dist = sqrt(dist2);
    ldir = dist > 0.0f ? ldir / dist : normal;
    float attn = 1.0f;
    if (attenuation == 1) {
        float cosine = max(0.0f, dot(ldir, light.direction.xyz));
        float cos_attn = dot(light.angular.xyz, float3(1.0f, cosine, cosine * cosine));
        float dist_attn = dot(light.distance.xyz, float3(1.0f, dist, dist2));
        attn = max(0.0f, cos_attn / dist_attn);
    } else if (attenuation == 0) {
        attn = dot(normal, ldir) >= 0.0f ? max(0.0f, dot(normal, light.direction.xyz)) : 0.0f;
        float cos_attn = dot(light.angular.xyz, float3(1.0f, attn, attn * attn));
        float3 coefficients = diffuse != 0 ? normalize(light.distance.xyz) : light.distance.xyz;
        float dist_attn = dot(coefficients, float3(1.0f, attn, attn * attn));
        attn = dist_attn != 0.0f ? max(0.0f, cos_attn / dist_attn) : (cos_attn > 0.0f ? 1.0f : 0.0f);
    }
    float diff = diffuse == 0 ? 1.0f : diffuse == 1 ? dot(ldir, normal) : max(0.0f, dot(ldir, normal));
    return attn * diff;
}
#endif
float4 light_channel(float3 position, float3 normal, float4 vertex) {
    int bits = draw.channel.z;
    float3 material = (bits & 4) != 0 ? vertex.rgb : draw.channel_material.rgb;
    float material_alpha = (bits & 16) != 0 ? vertex.a : draw.channel_material.a;
    float4 result = float4(material, material_alpha);
#if LIT == 1
    float3 ambient = (bits & 8) != 0 ? vertex.rgb : draw.channel_ambient.rgb;
    // A branch on a uniform keeps the program in parallel mode.
    if ((draw.channel.w & 0x10000) != 0) {
        for (int n = 0; n < LIGHTS; ++n)
            ambient += far_light((draw.channel.x >> (4 * n)) & 7, normal);
    } else {
        for (int n = 0; n < LIGHTS; ++n)
            ambient += spot_light((draw.channel.x >> (4 * n)) & 7, position, normal);
    }
    result.rgb = material * saturate(ambient);
#elif LIT == 2
    if ((bits & 1) != 0) {
        float3 accumulated = (bits & 8) != 0 ? vertex.rgb : draw.channel_ambient.rgb;
        for (int i = 0; i < 8; ++i)
            if ((draw.channel.x & (1 << i)) != 0)
                accumulated += light_term(i, position, normal, draw.channel.w & 15) * scene.lights[i].color.rgb;
        result.rgb = material * saturate(accumulated);
    }
    if ((bits & 2) != 0) {
        float accumulated = (bits & 32) != 0 ? vertex.a : draw.channel_ambient.a;
        for (int i = 0; i < 8; ++i)
            if ((draw.channel.y & (1 << i)) != 0)
                accumulated += light_term(i, position, normal, (draw.channel.w >> 8) & 15) * scene.lights[i].color.a;
        result.a = material_alpha * saturate(accumulated);
    }
#endif
    return result;
}
float fog_distance(float4 clip) {
    float depth = 1.0f - clip.z / clip.w;
    float ze = scene.fog_shape.z != 0.0f ? scene.fog.x * depth : scene.fog.x / (scene.fog.y - depth * scene.fog.w);
    return ze - scene.fog.z;
}
// The fog amount at a vertex (as the GLSL): the fragment blends by it.
float fog_amount(float4 clip) {
    float f = saturate(fog_distance(clip));
    float g = scene.fog_curve.x + scene.fog_curve.y * f;
    g = lerp(g, g * g, scene.fog_curve.z);
    return saturate(scene.fog_curve.w * f + scene.fog_shape.x + scene.fog_shape.y * exp2(-8.0f * g));
}

// The fog blend as the fragment takes it (as the GLSL fog_blend).
float4 fog_blend(float4 clip, inout float4 color) {
    float f = fog_amount(clip);
    float4 blend = float4(f * scene.fog_color.rgb, 1.0f - f);

    if ((draw.channel.w & 0x20000) != 0) {
        color.rgb *= blend.w;
        blend.w = 0.0f;
    }

    return blend;
}
)CG";

constexpr const char* CgMeshVertex = R"CG(
// mh3_shade_view: shared by the draws of one projection and viewport.
struct View {
    float4x4 proj;
    float4 viewport;
};
uniform View view : BUFFER[4];
#if SKINNED
// The mesh's palette rows (docs/optimizations/rendering/gpu-skinning.md): three float4 per matrix.
struct Palette {
    float4 rows[765];
};
#else
struct Palette {
    float4x3 position[20];
    float4x3 normal[10];
};
#endif
uniform Palette palette : BUFFER[2];
#if SKINNED
// m: the vertex's blended matrix rows; n: its normal (environment) matrix rows.
float2 texgen(int role, float2 uv, float4 m0, float4 m1, float4 m2, float3 n0, float3 n1, float3 n2, float3 position, float3 normal) {
#else
float2 texgen(int role, float2 uv, float selector, float3 position, float3 normal) {
#endif
    Texgen t = draw.texgen[role];
    int bits = t.mode.y;
    float4 source = t.mode.x == 0 ? float4(position, 1.0f) : t.mode.x == 1 ? float4(normal, 1.0f) : float4(uv, 1.0f, 1.0f);
    float3 coordinate = source.xyz;
    if ((bits & 16) != 0) {
#if SKINNED
        // 64: the environment matrix (the normal matrix, no translation).
        coordinate = (bits & 64) != 0 ? float3(dot(source.xyz, n0), dot(source.xyz, n1), dot(source.xyz, n2)) : float3(dot(source, m0), dot(source, m1), dot(source, m2));
#else
        int slot = int(selector);
        if (slot < 20)
            coordinate = mul(source, palette.position[slot]);
#endif
    } else if ((bits & 4) == 0)
        coordinate = mul(source, t.main);
    if ((bits & 1) == 0)
        coordinate.z = 1.0f;
    if ((bits & 2) != 0)
        coordinate = normalize(coordinate);
#if MATERIAL_GPU
    if ((bits & 32) != 0)
        coordinate.xy = srt2d(role, coordinate.xy);
    else
#endif
    if ((bits & 8) == 0)
        coordinate = mul(float4(coordinate, 1.0f), t.post);
    // Selects, not branches: psp2cgc turns a vector ?: or an if on a vertex's
    // values into dynamic flow control, which runs the program in per-instance
    // mode (one vertex per USSE thread instead of four).
    float projective = coordinate.z != 0.0f ? 1.0f : 0.0f;
    return lerp(clamp(coordinate.xy / 2.0f, -1.0f, 1.0f), coordinate.xy / (coordinate.z + (1.0f - projective)), projective);
}
float2 affine(int role, float2 uv) { return mul(float4(uv, 1.0f, 1.0f), draw.texgen[role].main).xy; }
void main(float3 a_position, float3 a_normal, float4 a_color0, float2 a_uv0, float2 a_uv1, float2 a_uv2,
#if SKINNED
          float4 a_bones, float4 a_weights,
#else
          float a_select, float a_select0, float a_select1, float a_select2,
#endif
          float4 out v_position : POSITION, float4 out v_color0 : COLOR0,
          float2 out v_uv0 : TEXCOORD0, float2 out v_uv1 : TEXCOORD1, float2 out v_uv2 : TEXCOORD2, float4 out v_fog : TEXCOORD3) {
#if SKINNED
    // M = sum weight * row matrix; its normal matrix is the inverse-transpose
    // (cofactor / det), the plain 3x3 when singular, as the game's view pass.
    int4 b = int4(a_bones) * 3;
    float4 w = a_weights;
    float4 m0 = w.x * palette.rows[b.x] + w.y * palette.rows[b.y] + w.z * palette.rows[b.z] + w.w * palette.rows[b.w];
    float4 m1 = w.x * palette.rows[b.x + 1] + w.y * palette.rows[b.y + 1] + w.z * palette.rows[b.z + 1] + w.w * palette.rows[b.w + 1];
    float4 m2 = w.x * palette.rows[b.x + 2] + w.y * palette.rows[b.y + 2] + w.z * palette.rows[b.z + 2] + w.w * palette.rows[b.w + 2];
    float3 n0 = cross(m1.xyz, m2.xyz);
    float3 n1 = cross(m2.xyz, m0.xyz);
    float3 n2 = cross(m0.xyz, m1.xyz);
    float det = dot(m0.xyz, n0);
    float singular = abs(det) < 1e-36f ? 1.0f : 0.0f;
    float inverse = 1.0f / (det + singular);
    n0 = lerp(n0 * inverse, m0.xyz, singular);
    n1 = lerp(n1 * inverse, m1.xyz, singular);
    n2 = lerp(n2 * inverse, m2.xyz, singular);
    float4 p = float4(a_position, 1.0f);
    float3 position = float3(dot(p, m0), dot(p, m1), dot(p, m2));
#else
    int slot = (draw.flags.x & 1) != 0 ? int(a_select) : 0;
    float3 position = mul(float4(a_position, 1.0f), palette.position[slot]);
#endif
    float4 projected = mul(float4(position, 1.0f), view.proj);
    float4 vertex = (draw.flags.x & 2) != 0 ? a_color0 : float4(1.0f, 1.0f, 1.0f, 1.0f);
#if LIT
#if SKINNED
    float3 normal = float3(dot(a_normal, n0), dot(a_normal, n1), dot(a_normal, n2));
#else
    float3 normal = mul(float4(a_normal, 0.0f), palette.normal[slot]);
#endif
    float length2 = dot(normal, normal);
    normal *= length2 > 1e-10f ? rsqrt(length2) : 1.0f;
    v_color0 = light_channel(position, normal, vertex);
#else
    v_color0 = light_channel(position, float3(0.0f, 0.0f, 0.0f), vertex);
#endif
    v_fog = fog_blend(projected, v_color0);
    projected.xy += projected.w * view.viewport.zw;
    TEXGEN_OUTPUTS
    v_position = float4(projected.x, projected.y, 2.0f * projected.z - projected.w, projected.w);
}
)CG";

constexpr const char* CgGeometryVertex = R"CG(
float2 affine(float3 uv) { return uv.z != 0.0f ? uv.xy / uv.z : uv.xy; }
void main(float4 a_position, float3 a_view_position, float3 a_normal, float4 a_color0, float3 a_uv0, float3 a_uv1, float3 a_uv2,
          float4 out v_position : POSITION, float4 out v_color0 : COLOR0,
          float2 out v_uv0 : TEXCOORD0, float2 out v_uv1 : TEXCOORD1, float2 out v_uv2 : TEXCOORD2, float4 out v_fog : TEXCOORD3) {
    v_color0 = light_channel(a_view_position, a_normal, a_color0);
    float4 p = a_position;
    v_fog = fog_blend(float4(p.xy, 0.5f * (p.z + p.w), p.w), v_color0);
    v_uv0 = affine(a_uv0);
    v_uv1 = affine(a_uv1);
    v_uv2 = affine(a_uv2);
    v_position = float4(p.x, p.y, p.z, p.w);
}
)CG";

// The Quad vertex program in Cg (GlslQuadVertex). The block's selects are on
// uniforms, so the program stays in parallel mode.
constexpr const char* CgQuadVertex = R"CG(
struct Quad {
    float4x3 position;
    float4x4 proj;
    float4 normal;
    float4 color0;
    int4 mode;
};
uniform Quad quad : BUFFER[2];
float2 affine(float3 uv) { return uv.z != 0.0f ? uv.xy / uv.z : uv.xy; }
float2 coordinate(int source, float3 uv0, float3 uv1) { return source == 0 ? affine(uv0) : source == 1 ? affine(uv1) : float2(0.0f, 0.0f); }
void main(float3 a_position, float3 a_uv0, float3 a_uv1,
#ifdef LERP
          float4 a_base, float4 a_slope, float4 out v_base : TEXCOORD4, float4 out v_slope : TEXCOORD5,
#endif
          float4 out v_position : POSITION, float4 out v_color0 : COLOR0,
          float2 out v_uv0 : TEXCOORD0, float2 out v_uv1 : TEXCOORD1, float2 out v_uv2 : TEXCOORD2, float4 out v_fog : TEXCOORD3) {
    float3 view = mul(float4(a_position, 1.0f), quad.position);
    float4 clip = mul(float4(view, 1.0f), quad.proj);
    float depth = quad.mode.w != 0 ? -clip.z : clip.z + clip.w;
    float4 p = float4(clip.x, clip.y, 2.0f * depth - clip.w, clip.w);
    v_color0 = light_channel(view, quad.normal.xyz, quad.color0);
    v_fog = fog_blend(float4(p.xy, 0.5f * (p.z + p.w), p.w), v_color0);
    v_uv0 = coordinate(quad.mode.x, a_uv0, a_uv1);
    v_uv1 = coordinate(quad.mode.y, a_uv0, a_uv1);
    v_uv2 = coordinate(quad.mode.z, a_uv0, a_uv1);
#ifdef LERP
    v_base = a_base;
    v_slope = a_slope;
#endif
    v_position = p;
}
)CG";

// Texture reads take the interpolated coordinate directly; LOD bias is in the
// texture control words, so no read depends on fragment arithmetic. Colours
// are computed in half precision (real), which halves the primary attributes
// and shortens most programs (psp2shaderperf: modulate_t1 12 -> 8 cycles,
// modulate_t1_fog_linear 19 -> 16, basic_t1_fog_linear 67 -> 51); half keeps
// 11 significant bits, the 8-bit target 8. Programs that evaluate picks stay
// in float: converting the picked coefficients costs them 1 to 8 cycles.
constexpr const char* CgFragment = R"CG(
#if DYNAMIC
#define real float
#define real2 float2
#define real3 float3
#define real4 float4
#else
#define real half
#define real2 half2
#define real3 half3
#define real4 half4
#endif
#if MATERIAL_GPU
real4 coefficient(int k) {
#if DYNAMIC
    Pick p = material.pick[k];
    real4 c = anim.palette[int(p.index.x)];
    real4 a = anim.palette[int(p.index.y)];
    return material.coefficient[k] * real4(lerp(c.rgb, dot(c, p.color_mask).xxx, p.index.z), dot(a, p.alpha_mask));
#else
    return material.coefficient[k];
#endif
}
#define ALPHA_RANGE anim.alpha_range
#else
#define ALPHA_RANGE shade.alpha_range
#endif
#if !MODULATE && !LAYERS
real4 source(real4 select, real4 t0, real4 t1, real4 t2, real4 v) {
    return select.x * t0 + select.y * t1 + select.z * t2 + select.w * v;
}
#if MATERIAL_GPU
real3 color_input(int i, real swizzle, real4 t0, real4 t1, real4 t2, real4 v) {
    real4 value = source(material.select[i], t0, t1, t2, v);
    return coefficient(i).rgb + coefficient(4 + i).rgb * lerp(value.rgb, value.aaa, swizzle);
}
real alpha_input(int i, real4 t0, real4 t1, real4 t2, real4 v) {
    return coefficient(i).a + coefficient(4 + i).a * source(material.select[4 + i], t0, t1, t2, v).a;
}
#define SWIZZLE0 material.swizzle.x
#define SWIZZLE1 material.swizzle.y
#define SWIZZLE2 material.swizzle.z
#define SWIZZLE3 material.swizzle.w
#define COLOR_EQUATION material.equation[0]
#define ALPHA_EQUATION material.equation[1]
#define OVERLAY_SCALE coefficient(8)
#define LERP_COLOR_OFFSET(i) coefficient(i)
#define LERP_ALPHA_OFFSET(i) coefficient(i).a
#define LERP_COLOR_SCALE coefficient(6)
#define LERP_ALPHA_SCALE coefficient(6).a
#define LERP_SWIZZLE material.swizzle.z
#else
real3 color_input(int i, real swizzle, real4 t0, real4 t1, real4 t2, real4 v) {
    Input s = shade.color_input[i];
    real4 value = source(s.select, t0, t1, t2, v);
    return s.offset.rgb + s.scale.rgb * lerp(value.rgb, value.aaa, real(s.swizzle.x));
}
real alpha_input(int i, real4 t0, real4 t1, real4 t2, real4 v) {
    Input s = shade.alpha_input[i];
    return s.offset.a + s.scale.a * source(s.select, t0, t1, t2, v).a;
}
#define SWIZZLE0 0.0h
#define SWIZZLE1 0.0h
#define SWIZZLE2 0.0h
#define SWIZZLE3 0.0h
#define COLOR_EQUATION shade.color_equation
#define ALPHA_EQUATION shade.alpha_equation
#define OVERLAY_SCALE shade.overlay
#define LERP_COLOR_OFFSET(i) shade.color_input[i].offset
#define LERP_ALPHA_OFFSET(i) shade.alpha_input[i].offset.a
#define LERP_COLOR_SCALE shade.color_input[2].scale
#define LERP_ALPHA_SCALE shade.alpha_input[2].scale.a
#define LERP_SWIZZLE shade.color_input[2].swizzle.x
#endif
#endif
real4 main(real4 v_color0 : COLOR0, float2 v_uv0 : TEXCOORD0, float2 v_uv1 : TEXCOORD1, float2 v_uv2 : TEXCOORD2, real4 v_fog : TEXCOORD3,
#ifdef VERTEX_LERP
            real4 v_base : TEXCOORD4, real4 v_slope : TEXCOORD5,
#endif
            uniform sampler2D u_texture0 : TEXUNIT0, uniform sampler2D u_texture1 : TEXUNIT1, uniform sampler2D u_texture2 : TEXUNIT2) : COLOR {
    real4 t0 = real4(1.0h, 1.0h, 1.0h, 1.0h), t1 = t0, t2 = t0;
#if TEXTURES > 0
    t0 = tex2D(u_texture0, v_uv0);
#endif
#if TEXTURES > 1
    t1 = tex2D(u_texture1, v_uv1);
#endif
#if TEXTURES > 2
    t2 = tex2D(u_texture2, v_uv2);
#endif
    real4 v = v_color0;
#if MODULATE && PURE_MODULATE && FOG
    // The vertices folded the fog blend into the colour (fog_blend): one
    // multiply-add, no saturation (texel and colour are in [0, 1], the colour
    // at most 1 - f). psp2shaderperf modulate_t1_pure_fog_linear 7 -> 1 cycle.
    real4 result = t0 * v + v_fog;
#elif MODULATE && PURE_MODULATE
    real4 result;
    result.rg = saturate(t0.rg * v.rg);
    result.ba = saturate(t0.ba * v.ba);
#elif MODULATE
#if MATERIAL_GPU
    real4 w0 = coefficient(0), w1 = coefficient(1), offset = coefficient(2), kv = coefficient(3), kc = coefficient(4);
    real4 d = coefficient(5) + material.add, s = material.scale, overlay = coefficient(6);
#else
    real4 w0 = shade.w0, w1 = shade.w1, offset = shade.offset, kv = shade.kv, kc = shade.kc, d = shade.d, s = shade.s, overlay = shade.overlay;
#endif
    // Keep each arithmetic chain two components wide. The float4 chain's
    // masked upper-lane uniform MAD loses B/A on the devkit; identical RG/BA
    // equations pass the isolated native Shade-buffer readback test.
    real2 base_rg = t0.rg * w0.rg + offset.rg;
    real2 base_ba = t0.ba * w0.ba + offset.ba;
#if TEXTURES == 2
    base_rg += t1.rg * w1.rg;
    base_ba += t1.ba * w1.ba;
#endif
#if OVERLAY
    base_rg += overlay.rg * t1.rg * t2.rg;
    base_ba.x += overlay.b * t1.b * t2.b;
#endif
    real4 result;
    result.rg = saturate((base_rg * (v.rg * kv.rg + kc.rg) + d.rg) * s.rg);
    result.ba = saturate((base_ba * (v.ba * kv.ba + kc.ba) + d.ba) * s.ba);
#elif LAYERS
    // Two components wide, as MODULATE.
    real2 sum_rg = (v.rg * shade.kv[0].rg + shade.kc[0].rg) * t0.rg + v.rg * shade.kv[3].rg + shade.kc[3].rg;
    real2 sum_ba = (v.ba * shade.kv[0].ba + shade.kc[0].ba) * t0.ba + v.ba * shade.kv[3].ba + shade.kc[3].ba;
#if TEXTURES > 1
    sum_rg += (v.rg * shade.kv[1].rg + shade.kc[1].rg) * t1.rg;
    sum_ba += (v.ba * shade.kv[1].ba + shade.kc[1].ba) * t1.ba;
#endif
#if TEXTURES > 2
    sum_rg += (v.rg * shade.kv[2].rg + shade.kc[2].rg) * t2.rg;
    sum_ba += (v.ba * shade.kv[2].ba + shade.kc[2].ba) * t2.ba;
#endif
    real4 result;
    result.rg = saturate(saturate(sum_rg * shade.s.rg) * (v.rg * shade.mv.rg + shade.mc.rg));
    result.ba = saturate(saturate(sum_ba * shade.s.ba) * (v.ba * shade.mv.ba + shade.mc.ba));
#elif BASIC_LERP && defined(VERTEX_LERP)
    // The quad's base and slope came with its vertices.
    real4 result = saturate(v_base + v_slope * real4(lerp(t0.rgb, t0.aaa, real(LERP_SWIZZLE)), t0.a));
#elif BASIC_LERP
    // Constant inputs are their offsets; only c (texture role 0) varies.
    // Affine in the texture (as the GLSL): psp2shaderperf basic_t1_lerp_gpu
    // 13 -> 7 cycles.
    real3 range = LERP_COLOR_OFFSET(1).rgb - LERP_COLOR_OFFSET(0).rgb;
    real3 slope = range * LERP_COLOR_SCALE.rgb * (COLOR_EQUATION.z * COLOR_EQUATION.y);
    real3 base = (LERP_COLOR_OFFSET(3).rgb + COLOR_EQUATION.x + COLOR_EQUATION.z * (LERP_COLOR_OFFSET(0).rgb + range * LERP_COLOR_OFFSET(2).rgb)) * COLOR_EQUATION.y;
    real alpha_range = LERP_ALPHA_OFFSET(1) - LERP_ALPHA_OFFSET(0);
    real alpha_slope = alpha_range * LERP_ALPHA_SCALE * (ALPHA_EQUATION.z * ALPHA_EQUATION.y);
    real alpha_base = (LERP_ALPHA_OFFSET(3) + ALPHA_EQUATION.x + ALPHA_EQUATION.z * (LERP_ALPHA_OFFSET(0) + alpha_range * LERP_ALPHA_OFFSET(2))) * ALPHA_EQUATION.y;
    real4 result = saturate(real4(base, alpha_base) + real4(slope, alpha_slope) * real4(lerp(t0.rgb, t0.aaa, real(LERP_SWIZZLE)), t0.a));
#else
    real3 b = color_input(1, SWIZZLE1, t0, t1, t2, v);
#if OVERLAY
    b += OVERLAY_SCALE.rgb * t1.rgb * t2.rgb;
#endif
    real3 color = lerp(color_input(0, SWIZZLE0, t0, t1, t2, v), b, color_input(2, SWIZZLE2, t0, t1, t2, v));
    color = (color_input(3, SWIZZLE3, t0, t1, t2, v) + COLOR_EQUATION.x + COLOR_EQUATION.z * color) * COLOR_EQUATION.y;
    real alpha = lerp(alpha_input(0, t0, t1, t2, v), alpha_input(1, t0, t1, t2, v), alpha_input(2, t0, t1, t2, v));
    alpha = (alpha_input(3, t0, t1, t2, v) + ALPHA_EQUATION.x + ALPHA_EQUATION.z * alpha) * ALPHA_EQUATION.y;
    real4 result = saturate(real4(color, alpha));
#endif
#if RASTER_PRODUCT
    // Two components wide, as MODULATE.
    result.rg *= v.rg;
    result.ba *= v.ba;
#endif
#if ALPHA_TEST
    if (result.a < ALPHA_RANGE.x || result.a >= ALPHA_RANGE.y)
        discard;
#endif
#if FOG && !(MODULATE && PURE_MODULATE)
    // f times the fog colour and 1 - f, as the vertices computed them
    // (fog_blend): one multiply-add instead of the blend's two
    // (modulate_t1_fog_linear 11 -> 10 cycles).
    result.rgb = result.rgb * v_fog.w + v_fog.rgb;
#endif
#if TARGET_ALPHA_ONE
    result.a = 1.0h;
#endif
    return result;
}
)CG";

// The CHAIN program in Cg (GlslChainFragment), half precision as the others.
constexpr const char* CgChainFragment = R"CG(
#define real half
#define real3 half3
#define real4 half4
struct Step {
    float4 keep_v;
    float4 keep_c;
    float4 keep_base;
    float4 layer_v;
    float4 layer_c;
    float4 mask_scale;
    float4 mask_offset;
    float4 s;
};
struct Shade {
    float4 texture_bias;
    float4 overlay;
    float4 alpha_range;
    float4 base_v;
    float4 base_c;
    float4 base_d;
    Step step[3];
    float4 alpha_sum;
    float4 alpha_sum2;
    float4 alpha_times;
    float4 alpha_times2;
    float4 alpha_factor;
    float4 mask_bias;
};
uniform Shade shade : BUFFER[0];
real3 chain_step(int i, real3 x, real3 base, real3 v, real3 layer, real3 mask) {
    real3 p = saturate((x * (v * real3(shade.step[i].keep_v.rgb) + real3(shade.step[i].keep_c.rgb)) + base +
                        layer * (v * real3(shade.step[i].layer_v.rgb) + real3(shade.step[i].layer_c.rgb))) * real3(shade.step[i].s.rgb));
    return lerp(x, p, mask * real3(shade.step[i].mask_scale.rgb) + real3(shade.step[i].mask_offset.rgb));
}
real4 main(real4 v_color0 : COLOR0, float2 v_uv0 : TEXCOORD0, float2 v_uv1 : TEXCOORD1, float2 v_uv2 : TEXCOORD2, real4 v_fog : TEXCOORD3,
            uniform sampler2D u_texture0 : TEXUNIT0, uniform sampler2D u_texture1 : TEXUNIT1, uniform sampler2D u_texture2 : TEXUNIT2,
            uniform sampler2D u_texture3 : TEXUNIT3, uniform sampler2D u_texture4 : TEXUNIT4) : COLOR {
    real4 one = real4(1.0h, 1.0h, 1.0h, 1.0h), t1 = one, t2 = one, t3 = one, t4 = one;
    real4 t0 = tex2D(u_texture0, v_uv0);
#if ROLES & 2
    t1 = tex2D(u_texture1, v_uv1);
#endif
#if ROLES & 4
    t2 = tex2D(u_texture2, v_uv2);
#endif
#if ROLES & 8
    t3 = tex2D(u_texture3, v_uv0);
#endif
#if ROLES & 16
    t4 = tex2D(u_texture4, v_uv0);
#endif
    real4 v = v_color0;
    real3 x = saturate(t0.rgb * (v.rgb * real3(shade.base_v.rgb) + real3(shade.base_c.rgb)) + real3(shade.base_d.rgb));
    real3 x0 = x, none = real3(0.0h, 0.0h, 0.0h);
#if STEPS > 0
    x = chain_step(0, x, (BASE & 1) != 0 ? x0 * real3(shade.step[0].keep_base.rgb) : none, v.rgb, LAYER0.rgb, MASK0.rgb);
#endif
#if STEPS > 1
    x = chain_step(1, x, (BASE & 2) != 0 ? x0 * real3(shade.step[1].keep_base.rgb) : none, v.rgb, LAYER1.rgb, MASK1.rgb);
#endif
#if STEPS > 2
    x = chain_step(2, x, (BASE & 4) != 0 ? x0 * real3(shade.step[2].keep_base.rgb) : none, v.rgb, LAYER2.rgb, MASK2.rgb);
#endif
    real4 alphas = real4(t0.a, t1.a, t2.a, t3.a);
    real3 others = real3(t4.a, v.a, 1.0h);
    real a = dot(real4(shade.alpha_sum), alphas) + dot(real3(shade.alpha_sum2.xyz), others);
#if ALPHA_SUM
    a = saturate(a) * (dot(real4(shade.alpha_times), alphas) + dot(real3(shade.alpha_times2.xyz), others));
#endif
    real4 result = real4(x, saturate(a * (v.a * real(shade.alpha_factor.x) + real(shade.alpha_factor.y))));
#if ALPHA_TEST
    if (result.a < shade.alpha_range.x || result.a >= shade.alpha_range.y)
        discard;
#endif
#if FOG
    result.rgb = result.rgb * v_fog.w + v_fog.rgb;
#endif
#if TARGET_ALPHA_ONE
    result.a = 1.0h;
#endif
    return result;
}
)CG";

std::string Define(const char* name, int value) {
    return std::string("#define ") + name + " " + std::to_string(value) + "\n";
}

// One statement per role; TEXGEN_OUTPUTS expands inside the mesh main().
// General texgens first: psp2cgc -O3 crashed (a null read in the compiler)
// on skinned programs with SRT roles 0 and 1 before a general role 2, and
// the bank fell back to -O2 (mesh_spot*_ssg_gpu_skin: 646.5 cycles; with the
// texgen first, -O3: 324.25).
std::string TexgenOutputs(const NativeShaderVertexKey& key, bool cg) {
    std::string out = "#define TEXGEN_OUTPUTS";
    std::array<unsigned, 3> order{0, 1, 2};
    std::stable_partition(order.begin(), order.end(), [&](unsigned role) { return key.texgen[role] == NativeShaderTexgen::General; });

    for (const unsigned role : order) {
        const auto r = std::to_string(role);
        out += " v_uv" + r + " = ";

        switch (key.texgen[role]) {
        case NativeShaderTexgen::None:
            out += cg ? "float2(0.0f, 0.0f)" : "vec2(0.0)";
            break;
        case NativeShaderTexgen::Identity:
            out += "a_uv" + r;
            break;
        case NativeShaderTexgen::Affine:
            out += "affine(" + r + ", a_uv" + r + ")";
            break;
        case NativeShaderTexgen::General:
            out += "texgen(" + r + ", a_uv" + r + (key.skinned ? ", m0, m1, m2, n0, n1, n2" : ", a_select" + r) + ", a_position, a_normal)";
            break;
        case NativeShaderTexgen::Srt:
            out += "srt2d(" + r + ", affine(" + r + ", a_uv" + r + "))";
            break;
        }

        out += ";";
    }

    return out + "\n";
}

std::string VertexDefines(const NativeShaderVertexKey& key) {
    return Define("LIT", int(key.lighting)) + Define("LIGHTS", key.lights) + Define("MATERIAL_GPU", key.material_gpu) + Define("SKINNED", key.skinned) + (key.lerp ? Define("LERP", 1) : "");
}

bool PureModulate(const NativeShaderVariant& variant) {
    return variant.pure_modulate && variant.model == NativeShaderModel::Modulate && variant.textures == 1 && !variant.overlay;
}

// Animated picks are evaluated by one-texture MODULATE programs only: their
// uniform-only coefficient code fits GXM's secondary program (psp2shaderperf:
// +0..1 cycle). BASIC already needs ~100 secondary attributes; its picks would
// move into the per-fragment program (77 -> 184 cycles), as do those of two-
// and three-texture MODULATE (21 -> 66, 23 -> 62), so such a material with
// animated picks gets a per-draw resolved copy of its Material block instead
// (ResolvesOnCpu, native_material.cpp). The pure product reads no coefficient.
bool Dynamic(const NativeShaderVariant& variant) {
    return variant.material_gpu && variant.dynamic && variant.model == NativeShaderModel::Modulate && !PureModulate(variant);
}

std::string FragmentDefines(const NativeShaderFragmentKey& key) {
    return Define("MODULATE", key.model == NativeShaderModel::Modulate) + Define("LAYERS", key.model == NativeShaderModel::Layers) + Define("TEXTURES", key.textures) + Define("OVERLAY", key.overlay) + Define("ALPHA_TEST", key.alpha_test) + Define("FOG", key.fog) +
           Define("LINEAR_FOG", key.linear_fog) + Define("PURE_MODULATE", key.pure_modulate) + Define("TARGET_ALPHA_ONE", key.target_alpha_one) + Define("MATERIAL_GPU", key.material_gpu) +
           Define("DYNAMIC", key.dynamic) + Define("BASIC_LERP", key.basic_lerp) + Define("RASTER_PRODUCT", key.raster_product) + (key.vertex_lerp ? Define("VERTEX_LERP", 1) : "");
}

std::string GlslVertex(const NativeShaderVertexKey& key) {
    const bool mesh = key.vertex == NativeShaderVertex::Mesh;
    const char* main = mesh ? GlslMeshVertex : key.vertex == NativeShaderVertex::Quad ? GlslQuadVertex : GlslGeometryVertex;
    return std::string(GlslPrelude) + VertexDefines(key) + (mesh ? TexgenOutputs(key, false) : "") + GlslScene + GlslDraw + (mesh ? GlslAnimated : "") + GlslVertexCommon + main;
}

// A CHAIN form's steps, roles and the layer and mask each step reads.
std::string ChainDefines(const NativeShaderFragmentKey& key) {
    const auto& form = mh3_shade_chain_forms[key.chain];
    const auto role = [](uint8_t r, bool alpha) { return r == MH3_SHADE_NO_ROLE ? std::string("one") : "t" + std::to_string(r) + (alpha ? ".aaaa" : ""); };
    std::string out = Define("STEPS", form.steps) + Define("ROLES", form.roles) + Define("BASE", form.base) + Define("ALPHA_SUM", form.alpha_sum) +
                      Define("ALPHA_TEST", key.alpha_test) + Define("FOG", key.fog) + Define("LINEAR_FOG", key.linear_fog) + Define("TARGET_ALPHA_ONE", key.target_alpha_one);

    for (unsigned i = 0; i < 3; ++i) {
        const auto n = std::to_string(i);
        const bool step = i < form.steps;
        out += "#define LAYER" + n + " " + role(step ? form.layer[i] : MH3_SHADE_NO_ROLE, false) + "\n";
        out += "#define MASK" + n + " " + role(step ? form.mask[i] : MH3_SHADE_NO_ROLE, step && (form.mask_alpha >> i & 1)) + "\n";
    }

    return out;
}

std::string GlslFragmentSource(const NativeShaderFragmentKey& key) {
    if (key.model == NativeShaderModel::Chain)
        return std::string(GlslPrelude) + ChainDefines(key) + GlslChainFragment;

    return std::string(GlslPrelude) + FragmentDefines(key) + GlslScene + GlslShade + GlslAnimated + GlslFragment;
}

[[noreturn]] void Invalid(const char* message) {
    MH3_THROW(std::invalid_argument(std::string("Native shader variant outside the bank domain: ") + message));
}
} // namespace

NativeShaderVertexKey NativeShaderVertexKeyOf(const NativeShaderVariant& variant) {
    NativeShaderVertexKey key;
    key.vertex = variant.vertex;
    key.lighting = variant.lighting;
    key.material_gpu = variant.material_gpu;

    if (variant.lighting == NativeShaderLighting::Spot) {
        if (variant.lights > NativeShaderMaxLights)
            Invalid("more than eight spot lights");

        key.lights = variant.lights;
    }

    key.skinned = variant.vertex == NativeShaderVertex::Mesh && variant.skinned;
    key.lerp = variant.vertex == NativeShaderVertex::Quad && variant.material_gpu && variant.vertex_lerp;

    if (variant.vertex == NativeShaderVertex::Mesh)
        for (unsigned role = 0; role < 3; ++role) {
            if (variant.texgen[role] == NativeShaderTexgen::Srt && !variant.material_gpu)
                Invalid("an SRT texgen without material uniforms");

            key.texgen[role] = variant.texgen[role];
        }
    return key;
}

bool NativeShaderFoldsFog(const NativeShaderVariant& variant) {
    return variant.fog && PureModulate(variant);
}

// The roles a CHAIN form samples: the highest one and those below it.
unsigned ChainTextures(unsigned chain) {
    unsigned textures = 0;

    for (unsigned r = 0; r < NativeShaderChainTextures; ++r)
        if (mh3_shade_chain_forms[chain].roles & (1u << r))
            textures = r + 1;

    return textures;
}

NativeShaderFragmentKey NativeShaderFragmentKeyOf(const NativeShaderVariant& variant) {
    if (variant.model == NativeShaderModel::Chain) {
        if (variant.chain >= MH3_SHADE_CHAIN_FORMS)
            Invalid("a chain form outside mh3_shade_chain_forms");

        // Only the form and the alpha, fog and target options reach the program.
        NativeShaderFragmentKey key;
        key.model = variant.model;
        key.chain = variant.chain;
        key.textures = uint8_t(ChainTextures(variant.chain));
        key.alpha_test = variant.alpha_test;
        key.fog = variant.fog;
        key.linear_fog = variant.fog && variant.linear_fog;
        key.target_alpha_one = variant.target_alpha_one;
        return key;
    }

    if (variant.textures > NativeShaderMaxTextures)
        Invalid("more than three texture roles");

    NativeShaderFragmentKey key;
    key.model = variant.model;
    key.textures = variant.textures;
    key.overlay = variant.overlay;
    key.alpha_test = variant.alpha_test;
    key.fog = variant.fog;
    key.linear_fog = variant.fog && variant.linear_fog;
    key.pure_modulate = PureModulate(variant);
    key.target_alpha_one = variant.target_alpha_one;
    key.material_gpu = variant.material_gpu;
    key.dynamic = Dynamic(variant);
    key.basic_lerp = variant.basic_lerp && variant.model == NativeShaderModel::Basic && variant.textures > 0 && !variant.overlay;
    key.raster_product = variant.raster_product && variant.model == NativeShaderModel::Basic && !variant.overlay;
    key.vertex_lerp = variant.vertex_lerp && key.basic_lerp && variant.material_gpu;

    // LAYERS has no Material block (mh3_shade_gpu_bake) and no overlay.
    if (variant.model == NativeShaderModel::Layers)
        key.overlay = key.material_gpu = key.dynamic = false;

    return key;
}

// Every value of every variant field: the image of the key functions over all
// valid variants. A key the runtime classifies is always one of these.
std::vector<NativeShaderVertexKey> NativeShaderVertexKeys() {
    std::set<NativeShaderVertexKey> seen;
    std::vector<NativeShaderVertexKey> keys;
    NativeShaderVariant v;

    for (unsigned vertex = 0; vertex < NativeShaderVertexCount; ++vertex)
        for (unsigned lighting = 0; lighting < NativeShaderLightingCount; ++lighting)
            for (unsigned lights = 0; lights <= NativeShaderMaxLights; ++lights)
                for (unsigned flags = 0; flags < 8; ++flags)
                    for (unsigned t0 = 0; t0 < NativeShaderTexgenCount; ++t0)
                        for (unsigned t1 = 0; t1 < NativeShaderTexgenCount; ++t1)
                            for (unsigned t2 = 0; t2 < NativeShaderTexgenCount; ++t2) {
                                v.vertex = NativeShaderVertex(vertex);
                                v.lighting = NativeShaderLighting(lighting);
                                v.lights = uint8_t(lights);
                                v.material_gpu = flags & 1;
                                v.skinned = flags & 2;
                                v.vertex_lerp = flags & 4;
                                v.texgen[0] = NativeShaderTexgen(t0);
                                v.texgen[1] = NativeShaderTexgen(t1);
                                v.texgen[2] = NativeShaderTexgen(t2);
                                const bool srt = v.texgen[0] == NativeShaderTexgen::Srt || v.texgen[1] == NativeShaderTexgen::Srt || v.texgen[2] == NativeShaderTexgen::Srt;

                                if (srt && !v.material_gpu && v.vertex == NativeShaderVertex::Mesh)
                                    continue; // rejected by NativeShaderVertexKeyOf
                                const auto key = NativeShaderVertexKeyOf(v);

                                if (seen.insert(key).second)
                                    keys.push_back(key);
                            }
    return keys;
}

std::vector<NativeShaderFragmentKey> NativeShaderFragmentKeys() {
    std::set<NativeShaderFragmentKey> seen;
    std::vector<NativeShaderFragmentKey> keys;
    NativeShaderVariant v;

    for (unsigned model = 0; model < NativeShaderModelCount; ++model)
        for (unsigned textures = 0; textures <= NativeShaderMaxTextures; ++textures)
            for (unsigned chain = 0; chain < (model == unsigned(NativeShaderModel::Chain) ? MH3_SHADE_CHAIN_FORMS : 1u); ++chain)
            for (unsigned flags = 0; flags < 2048; ++flags) {
                v.model = NativeShaderModel(model);
                v.textures = uint8_t(textures);
                v.chain = uint8_t(chain);
                v.overlay = flags & 1;
                v.alpha_test = flags & 2;
                v.fog = flags & 4;
                v.linear_fog = flags & 8;
                v.pure_modulate = flags & 16;
                v.target_alpha_one = flags & 32;
                v.material_gpu = flags & 64;
                v.dynamic = flags & 128;
                v.basic_lerp = flags & 256;
                v.raster_product = flags & 512;
                v.vertex_lerp = flags & 1024;
                const auto key = NativeShaderFragmentKeyOf(v);

                if (seen.insert(key).second)
                    keys.push_back(key);
            }
    return keys;
}

std::string NativeShaderCgVertex(const NativeShaderVertexKey& key) {
    const bool mesh = key.vertex == NativeShaderVertex::Mesh;
    const char* main = mesh ? CgMeshVertex : key.vertex == NativeShaderVertex::Quad ? CgQuadVertex : CgGeometryVertex;
    return std::string(CgScene) + VertexDefines(key) + (mesh ? TexgenOutputs(key, true) : "") + CgDraw + (mesh ? CgAnimated : "") + CgVertexCommon + main;
}

std::string NativeShaderCgFragment(const NativeShaderFragmentKey& key) {
    if (key.model == NativeShaderModel::Chain)
        return ChainDefines(key) + CgChainFragment;

    return std::string(CgScene) + FragmentDefines(key) + CgShade + CgAnimated + CgFragment;
}

#ifdef MH3_WEB_SHADER_BANK
std::string NativeShaderGlslVertex(const NativeShaderVertexKey& key) {
    return GlslVertex(key);
}

std::string NativeShaderGlslFragment(const NativeShaderFragmentKey& key) {
    return GlslFragmentSource(key);
}
#endif

NativeShaderSources NativeShaderSource(NativeShaderVariant variant) {
    const auto vertex = NativeShaderVertexKeyOf(variant);
    const auto fragment = NativeShaderFragmentKeyOf(variant);
    NativeShaderSources out;
    out.glsl_vertex = GlslVertex(vertex);
    out.glsl_fragment = GlslFragmentSource(fragment);
    out.cg_vertex = NativeShaderCgVertex(vertex);
    out.cg_fragment = NativeShaderCgFragment(fragment);
    return out;
}

std::string NativeShaderVertexName(const NativeShaderVertexKey& key) {
    static constexpr const char* lighting[NativeShaderLightingCount] = {"", "_spot", "_lit"};
    static constexpr char texgens[] = "niags";
    static_assert(sizeof(texgens) - 1 == NativeShaderTexgenCount);
    std::string light = lighting[unsigned(key.lighting)];

    if (key.lighting == NativeShaderLighting::Spot)
        light += std::to_string(key.lights);

    if (key.vertex != NativeShaderVertex::Mesh)
        return (key.vertex == NativeShaderVertex::Quad ? "quad" : "geometry") + light + (key.material_gpu ? "_gpu" : "") + (key.lerp ? "_lerp" : "");

    std::string name = "mesh" + light + "_";

    for (const auto texgen : key.texgen)
        name += texgens[unsigned(texgen)];

    return name + (key.material_gpu ? "_gpu" : "") + (key.skinned ? "_skin" : "");
}

std::string NativeShaderFragmentName(const NativeShaderFragmentKey& key) {
    static constexpr const char* models[NativeShaderModelCount] = {"basic", "modulate", "layers", "chain"};
    return std::string(models[unsigned(key.model)]) + (key.model == NativeShaderModel::Chain ? std::to_string(key.chain) : "") + "_t" + std::to_string(key.textures) +
           (key.overlay ? "_overlay" : "") + (key.pure_modulate ? "_pure" : "") +
           (key.alpha_test ? "_alpha" : "") + (key.fog ? (key.linear_fog ? "_fog_linear" : "_fog") : "") + (key.target_alpha_one ? "_a1" : "") +
           (key.basic_lerp ? (key.vertex_lerp ? "_vlerp" : "_lerp") : "") + (key.raster_product ? "_ras" : "") + (key.material_gpu ? (key.dynamic ? "_gpu_dyn" : "_gpu") : "");
}

std::string NativeShaderVertexName(NativeShaderVariant variant) {
    return NativeShaderVertexName(NativeShaderVertexKeyOf(variant));
}

std::string NativeShaderFragmentName(NativeShaderVariant variant) {
    return NativeShaderFragmentName(NativeShaderFragmentKeyOf(variant));
}
