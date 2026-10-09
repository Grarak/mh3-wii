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
// BUFFER[1]), Palette (mesh vertex, binding 2 / vertex BUFFER[2]) and Shade
// (fragment, binding 3 / fragment BUFFER[0]). Draw and Shade share one per-draw
// allocation (mh3_shade_draw); Shade's tail differs per fragment model.
// MATERIAL_GPU programs (docs/native-material-gpu.md) replace Shade with the
// baked Material block (binding 4 / fragment BUFFER[2]) and the Animated
// block (binding 5 / fragment BUFFER[3], mesh vertex BUFFER[3]); coefficients
// are static * picked animated palette entries, evaluated from uniforms only.
#include "native_shader_source.h"
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
    mat4 proj;
    vec4 viewport;
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
// << 2, alpha in bits 8..11). LIT 1 is colour-only spot lighting over LIGHTS
// lights; LIT 2 evaluates any GX channel.
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
    for (int n = 0; n < LIGHTS; ++n)
        ambient += spot_light((draw.channel.x >> (4 * n)) & 7, position, normal);
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
)GLSL";

constexpr const char* GlslMeshVertex = R"GLSL(
layout(std140) uniform Palette {
    mat3x4 position[20];
    mat3x4 normal[10];
} palette;
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
out float v_fog;
// mode.x source: 0 model position, 1 model normal, 2 role coordinate attribute.
// mode.y bits: 1 projective (three-row result), 2 normalize, 4 main identity,
// 8 post identity, 16 main from the palette slot in the role selector, 32 post
// from the role's Animated SRT (MATERIAL_GPU).
vec2 texgen(int role, vec2 uv, float selector, vec3 position, vec3 normal) {
    Texgen t = draw.texgen[role];
    int bits = t.mode.y;
    vec4 source = t.mode.x == 0 ? vec4(position, 1.0) : t.mode.x == 1 ? vec4(normal, 1.0) : vec4(uv, 1.0, 1.0);
    vec3 coordinate = source.xyz;
    if ((bits & 16) != 0) {
        int slot = int(selector);
        if (slot < 20)
            coordinate = source * palette.position[slot];
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
    int slot = (draw.flags.x & 1) != 0 ? int(a_select) : 0;
    vec3 position = vec4(a_position, 1.0) * palette.position[slot];
    vec4 projected = vec4(position, 1.0) * draw.proj;
    vec4 vertex = (draw.flags.x & 2) != 0 ? a_color0 : vec4(1.0);
#if LIT
    vec3 normal = vec4(a_normal, 0.0) * palette.normal[slot];
    normal = dot(normal, normal) > 1e-10 ? normalize(normal) : normal;
    v_color0 = light_channel(position, normal, vertex);
#else
    v_color0 = light_channel(position, vec3(0.0), vertex);
#endif
    v_fog = fog_distance(projected);
    projected.xy += projected.w * draw.viewport.zw;
    TEXGEN_OUTPUTS
    gl_Position = vec4(projected.x, -projected.y, 2.0 * projected.z - projected.w, projected.w);
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
out float v_fog;
vec2 affine(vec3 uv) { return uv.z != 0.0 ? uv.xy / uv.z : uv.xy; }
void main() {
    v_color0 = light_channel(a_view_position, a_normal, a_color0);
    // a_position is GL clip space (z in [-w, w]); fog reads the GX depth.
    vec4 p = a_position;
    v_fog = fog_distance(vec4(p.xy, 0.5 * (p.z + p.w), p.w));
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
constexpr const char* GlslFragment = R"GLSL(
in vec4 v_color0;
in vec2 v_uv0;
in vec2 v_uv1;
in vec2 v_uv2;
in float v_fog;
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
#if !MODULATE
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
#if MODULATE && PURE_MODULATE
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
#if ALPHA_TEST
    if (result.a < ALPHA_RANGE.x || result.a >= ALPHA_RANGE.y)
        discard;
#endif
#if FOG
    float f = clamp(v_fog, 0.0, 1.0);
#if LINEAR_FOG
    float amount = scene.fog_curve.w * f + scene.fog_shape.x;
#else
    float g = scene.fog_curve.x + scene.fog_curve.y * f;
    g = mix(g, g * g, scene.fog_curve.z);
    float amount = scene.fog_curve.w * f + scene.fog_shape.x + scene.fog_shape.y * exp2(-8.0 * g);
#endif
    result.rgb = mix(result.rgb, scene.fog_color.rgb, clamp(amount, 0.0, 1.0));
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
    float4x4 proj;
    float4 viewport;
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
    for (int n = 0; n < LIGHTS; ++n)
        ambient += spot_light((draw.channel.x >> (4 * n)) & 7, position, normal);
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
)CG";

constexpr const char* CgMeshVertex = R"CG(
struct Palette {
    float4x3 position[20];
    float4x3 normal[10];
};
uniform Palette palette : BUFFER[2];
float2 texgen(int role, float2 uv, float selector, float3 position, float3 normal) {
    Texgen t = draw.texgen[role];
    int bits = t.mode.y;
    float4 source = t.mode.x == 0 ? float4(position, 1.0f) : t.mode.x == 1 ? float4(normal, 1.0f) : float4(uv, 1.0f, 1.0f);
    float3 coordinate = source.xyz;
    if ((bits & 16) != 0) {
        int slot = int(selector);
        if (slot < 20)
            coordinate = mul(source, palette.position[slot]);
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
    return coordinate.z != 0.0f ? coordinate.xy / coordinate.z : clamp(coordinate.xy / 2.0f, -1.0f, 1.0f);
}
float2 affine(int role, float2 uv) { return mul(float4(uv, 1.0f, 1.0f), draw.texgen[role].main).xy; }
void main(float3 a_position, float3 a_normal, float4 a_color0, float2 a_uv0, float2 a_uv1, float2 a_uv2,
          float a_select, float a_select0, float a_select1, float a_select2,
          float4 out v_position : POSITION, float4 out v_color0 : COLOR0,
          float2 out v_uv0 : TEXCOORD0, float2 out v_uv1 : TEXCOORD1, float2 out v_uv2 : TEXCOORD2, float out v_fog : TEXCOORD3) {
    int slot = (draw.flags.x & 1) != 0 ? int(a_select) : 0;
    float3 position = mul(float4(a_position, 1.0f), palette.position[slot]);
    float4 projected = mul(float4(position, 1.0f), draw.proj);
    float4 vertex = (draw.flags.x & 2) != 0 ? a_color0 : float4(1.0f, 1.0f, 1.0f, 1.0f);
#if LIT
    float3 normal = mul(float4(a_normal, 0.0f), palette.normal[slot]);
    normal = dot(normal, normal) > 1e-10f ? normalize(normal) : normal;
    v_color0 = light_channel(position, normal, vertex);
#else
    v_color0 = light_channel(position, float3(0.0f, 0.0f, 0.0f), vertex);
#endif
    v_fog = fog_distance(projected);
    projected.xy += projected.w * draw.viewport.zw;
    TEXGEN_OUTPUTS
    v_position = float4(projected.x, projected.y, 2.0f * projected.z - projected.w, projected.w);
}
)CG";

constexpr const char* CgGeometryVertex = R"CG(
float2 affine(float3 uv) { return uv.z != 0.0f ? uv.xy / uv.z : uv.xy; }
void main(float4 a_position, float3 a_view_position, float3 a_normal, float4 a_color0, float3 a_uv0, float3 a_uv1, float3 a_uv2,
          float4 out v_position : POSITION, float4 out v_color0 : COLOR0,
          float2 out v_uv0 : TEXCOORD0, float2 out v_uv1 : TEXCOORD1, float2 out v_uv2 : TEXCOORD2, float out v_fog : TEXCOORD3) {
    v_color0 = light_channel(a_view_position, a_normal, a_color0);
    float4 p = a_position;
    v_fog = fog_distance(float4(p.xy, 0.5f * (p.z + p.w), p.w));
    v_uv0 = affine(a_uv0);
    v_uv1 = affine(a_uv1);
    v_uv2 = affine(a_uv2);
    v_position = float4(p.x, p.y, p.z, p.w);
}
)CG";

// Texture reads take the interpolated coordinate directly; LOD bias is in the
// texture control words, so no read depends on fragment arithmetic.
constexpr const char* CgFragment = R"CG(
#if MATERIAL_GPU
float4 coefficient(int k) {
#if DYNAMIC
    Pick p = material.pick[k];
    float4 c = anim.palette[int(p.index.x)];
    float4 a = anim.palette[int(p.index.y)];
    return material.coefficient[k] * float4(lerp(c.rgb, dot(c, p.color_mask).xxx, p.index.z), dot(a, p.alpha_mask));
#else
    return material.coefficient[k];
#endif
}
#define ALPHA_RANGE anim.alpha_range
#else
#define ALPHA_RANGE shade.alpha_range
#endif
#if !MODULATE
float4 source(float4 select, float4 t0, float4 t1, float4 t2, float4 v) {
    return select.x * t0 + select.y * t1 + select.z * t2 + select.w * v;
}
#if MATERIAL_GPU
float3 color_input(int i, float swizzle, float4 t0, float4 t1, float4 t2, float4 v) {
    float4 value = source(material.select[i], t0, t1, t2, v);
    return coefficient(i).rgb + coefficient(4 + i).rgb * lerp(value.rgb, value.aaa, swizzle);
}
float alpha_input(int i, float4 t0, float4 t1, float4 t2, float4 v) {
    return coefficient(i).a + coefficient(4 + i).a * source(material.select[4 + i], t0, t1, t2, v).a;
}
#define SWIZZLE0 material.swizzle.x
#define SWIZZLE1 material.swizzle.y
#define SWIZZLE2 material.swizzle.z
#define SWIZZLE3 material.swizzle.w
#define COLOR_EQUATION material.equation[0]
#define ALPHA_EQUATION material.equation[1]
#define OVERLAY_SCALE coefficient(8)
#else
float3 color_input(int i, float swizzle, float4 t0, float4 t1, float4 t2, float4 v) {
    Input s = shade.color_input[i];
    float4 value = source(s.select, t0, t1, t2, v);
    return s.offset.rgb + s.scale.rgb * lerp(value.rgb, value.aaa, s.swizzle.x);
}
float alpha_input(int i, float4 t0, float4 t1, float4 t2, float4 v) {
    Input s = shade.alpha_input[i];
    return s.offset.a + s.scale.a * source(s.select, t0, t1, t2, v).a;
}
#define SWIZZLE0 0.0f
#define SWIZZLE1 0.0f
#define SWIZZLE2 0.0f
#define SWIZZLE3 0.0f
#define COLOR_EQUATION shade.color_equation
#define ALPHA_EQUATION shade.alpha_equation
#define OVERLAY_SCALE shade.overlay
#endif
#endif
float4 main(float4 v_color0 : COLOR0, float2 v_uv0 : TEXCOORD0, float2 v_uv1 : TEXCOORD1, float2 v_uv2 : TEXCOORD2, float v_fog : TEXCOORD3,
            uniform sampler2D u_texture0 : TEXUNIT0, uniform sampler2D u_texture1 : TEXUNIT1, uniform sampler2D u_texture2 : TEXUNIT2) : COLOR {
    float4 t0 = float4(1.0f, 1.0f, 1.0f, 1.0f), t1 = t0, t2 = t0;
#if TEXTURES > 0
    t0 = tex2D(u_texture0, v_uv0);
#endif
#if TEXTURES > 1
    t1 = tex2D(u_texture1, v_uv1);
#endif
#if TEXTURES > 2
    t2 = tex2D(u_texture2, v_uv2);
#endif
    float4 v = v_color0;
#if MODULATE && PURE_MODULATE
    float4 result;
    result.rg = saturate(t0.rg * v.rg);
    result.ba = saturate(t0.ba * v.ba);
#elif MODULATE
#if MATERIAL_GPU
    float4 w0 = coefficient(0), w1 = coefficient(1), offset = coefficient(2), kv = coefficient(3), kc = coefficient(4);
    float4 d = coefficient(5) + material.add, s = material.scale, overlay = coefficient(6);
#else
    float4 w0 = shade.w0, w1 = shade.w1, offset = shade.offset, kv = shade.kv, kc = shade.kc, d = shade.d, s = shade.s, overlay = shade.overlay;
#endif
    // Keep each arithmetic chain two components wide. The float4 chain's
    // masked upper-lane uniform MAD loses B/A on the devkit; identical RG/BA
    // equations pass the isolated native Shade-buffer readback test.
    float2 base_rg = t0.rg * w0.rg + offset.rg;
    float2 base_ba = t0.ba * w0.ba + offset.ba;
#if TEXTURES == 2
    base_rg += t1.rg * w1.rg;
    base_ba += t1.ba * w1.ba;
#endif
#if OVERLAY
    base_rg += overlay.rg * t1.rg * t2.rg;
    base_ba.x += overlay.b * t1.b * t2.b;
#endif
    float4 result;
    result.rg = saturate((base_rg * (v.rg * kv.rg + kc.rg) + d.rg) * s.rg);
    result.ba = saturate((base_ba * (v.ba * kv.ba + kc.ba) + d.ba) * s.ba);
#else
    float3 b = color_input(1, SWIZZLE1, t0, t1, t2, v);
#if OVERLAY
    b += OVERLAY_SCALE.rgb * t1.rgb * t2.rgb;
#endif
    float3 color = lerp(color_input(0, SWIZZLE0, t0, t1, t2, v), b, color_input(2, SWIZZLE2, t0, t1, t2, v));
    color = (color_input(3, SWIZZLE3, t0, t1, t2, v) + COLOR_EQUATION.x + COLOR_EQUATION.z * color) * COLOR_EQUATION.y;
    float alpha = lerp(alpha_input(0, t0, t1, t2, v), alpha_input(1, t0, t1, t2, v), alpha_input(2, t0, t1, t2, v));
    alpha = (alpha_input(3, t0, t1, t2, v) + ALPHA_EQUATION.x + ALPHA_EQUATION.z * alpha) * ALPHA_EQUATION.y;
    float4 result = saturate(float4(color, alpha));
#endif
#if ALPHA_TEST
    if (result.a < ALPHA_RANGE.x || result.a >= ALPHA_RANGE.y)
        discard;
#endif
#if FOG
    float f = saturate(v_fog);
#if LINEAR_FOG
    float amount = scene.fog_curve.w * f + scene.fog_shape.x;
#else
    float g = scene.fog_curve.x + scene.fog_curve.y * f;
    g = lerp(g, g * g, scene.fog_curve.z);
    float amount = scene.fog_curve.w * f + scene.fog_shape.x + scene.fog_shape.y * exp2(-8.0f * g);
#endif
    result.rgb = lerp(result.rgb, scene.fog_color.rgb, saturate(amount));
#endif
#if TARGET_ALPHA_ONE
    result.a = 1.0f;
#endif
    return result;
}
)CG";

std::string Define(const char* name, int value) {
    return std::string("#define ") + name + " " + std::to_string(value) + "\n";
}
// One statement per role; TEXGEN_OUTPUTS expands inside the mesh main().
std::string TexgenOutputs(const NativeShaderVertexKey& key, bool cg) {
    std::string out = "#define TEXGEN_OUTPUTS";
    for (unsigned role = 0; role < 3; ++role) {
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
            out += "texgen(" + r + ", a_uv" + r + ", a_select" + r + ", a_position, a_normal)";
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
    return Define("LIT", int(key.lighting)) + Define("LIGHTS", key.lights) + Define("MATERIAL_GPU", key.material_gpu);
}
bool PureModulate(const NativeShaderVariant& variant) {
    return variant.pure_modulate && variant.model == NativeShaderModel::Modulate && variant.textures == 1 && !variant.overlay;
}
// Animated picks are evaluated by MODULATE programs only: their uniform-only
// coefficient code fits GXM's secondary program (psp2shaderperf: +0..1 cycle).
// BASIC already needs ~100 secondary attributes; its picks would move into the
// per-fragment program (77 -> 184 cycles), so a BASIC material with animated
// picks gets a per-draw resolved copy of its Material block instead. The pure
// product reads no coefficient.
bool Dynamic(const NativeShaderVariant& variant) {
    return variant.material_gpu && variant.dynamic && variant.model == NativeShaderModel::Modulate && !PureModulate(variant);
}
std::string FragmentDefines(const NativeShaderFragmentKey& key) {
    return Define("MODULATE", key.model == NativeShaderModel::Modulate) + Define("TEXTURES", key.textures) + Define("OVERLAY", key.overlay) + Define("ALPHA_TEST", key.alpha_test) + Define("FOG", key.fog) +
           Define("LINEAR_FOG", key.linear_fog) + Define("PURE_MODULATE", key.pure_modulate) + Define("TARGET_ALPHA_ONE", key.target_alpha_one) + Define("MATERIAL_GPU", key.material_gpu) +
           Define("DYNAMIC", key.dynamic);
}
std::string GlslVertex(const NativeShaderVertexKey& key) {
    const bool mesh = key.vertex == NativeShaderVertex::Mesh;
    return std::string(GlslPrelude) + VertexDefines(key) + (mesh ? TexgenOutputs(key, false) : "") + GlslScene + GlslDraw + (mesh ? GlslAnimated : "") + GlslVertexCommon +
           (mesh ? GlslMeshVertex : GlslGeometryVertex);
}
std::string GlslFragmentSource(const NativeShaderFragmentKey& key) {
    return std::string(GlslPrelude) + FragmentDefines(key) + GlslScene + GlslShade + GlslAnimated + GlslFragment;
}
[[noreturn]] void Invalid(const char* message) {
    throw std::invalid_argument(std::string("Native shader variant outside the bank domain: ") + message);
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
    if (variant.vertex == NativeShaderVertex::Mesh)
        for (unsigned role = 0; role < 3; ++role) {
            if (variant.texgen[role] == NativeShaderTexgen::Srt && !variant.material_gpu)
                Invalid("an SRT texgen without material uniforms");
            key.texgen[role] = variant.texgen[role];
        }
    return key;
}

NativeShaderFragmentKey NativeShaderFragmentKeyOf(const NativeShaderVariant& variant) {
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
                for (unsigned gpu = 0; gpu < 2; ++gpu)
                    for (unsigned t0 = 0; t0 < NativeShaderTexgenCount; ++t0)
                        for (unsigned t1 = 0; t1 < NativeShaderTexgenCount; ++t1)
                            for (unsigned t2 = 0; t2 < NativeShaderTexgenCount; ++t2) {
                                v.vertex = NativeShaderVertex(vertex);
                                v.lighting = NativeShaderLighting(lighting);
                                v.lights = uint8_t(lights);
                                v.material_gpu = gpu != 0;
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
            for (unsigned flags = 0; flags < 256; ++flags) {
                v.model = NativeShaderModel(model);
                v.textures = uint8_t(textures);
                v.overlay = flags & 1;
                v.alpha_test = flags & 2;
                v.fog = flags & 4;
                v.linear_fog = flags & 8;
                v.pure_modulate = flags & 16;
                v.target_alpha_one = flags & 32;
                v.material_gpu = flags & 64;
                v.dynamic = flags & 128;
                const auto key = NativeShaderFragmentKeyOf(v);
                if (seen.insert(key).second)
                    keys.push_back(key);
            }
    return keys;
}

std::string NativeShaderCgVertex(const NativeShaderVertexKey& key) {
    const bool mesh = key.vertex == NativeShaderVertex::Mesh;
    return std::string(CgScene) + VertexDefines(key) + (mesh ? TexgenOutputs(key, true) : "") + CgDraw + (mesh ? CgAnimated : "") + CgVertexCommon + (mesh ? CgMeshVertex : CgGeometryVertex);
}

std::string NativeShaderCgFragment(const NativeShaderFragmentKey& key) {
    return std::string(CgScene) + FragmentDefines(key) + CgShade + CgAnimated + CgFragment;
}

NativeShaderSources NativeShaderSource(NativeShaderVariant variant) {
    const auto vertex = NativeShaderVertexKeyOf(variant);
    const auto fragment = NativeShaderFragmentKeyOf(variant);
    NativeShaderSources out;
    out.glsl_vertex = GlslVertex(vertex);
    out.glsl_fragment = GlslFragmentSource(fragment);
    out.cg_vertex = NativeShaderCgVertex(vertex);
    out.cg_fragment = NativeShaderCgFragment(fragment);
#if defined(__vita__) && defined(MH3_DEVELOPMENT)
    // Diagnostic stage isolation; source identity changes, so a probe never
    // reuses a banked or cached production GXP (it compiles at runtime, a
    // reported bank miss). All shader arithmetic remains float.
    if (const char* probe = std::getenv("MH3_GXM_SHADE_PROBE")) {
        const bool mesh = variant.vertex == NativeShaderVertex::Mesh;
        std::string result;
        if (std::string(probe) == "blue")
            result = "float4(0.0f, 0.0f, 1.0f, 1.0f)";
        else if (std::string(probe) == "texture")
            result = "t0";
        else if (std::string(probe) == "vertex" || std::string(probe) == "vertex-white")
            result = "v_color0";
        else if (std::string(probe) == "geometry-texture" && !mesh && variant.textures)
            result = "t0";
        // Geometry includes full-screen composition and fades. Keep those
        // authored equations when isolating mesh vertex lighting.
        if (!result.empty() && ((std::string(probe) != "vertex" && std::string(probe) != "vertex-white") || mesh)) {
            if (variant.target_alpha_one)
                result = "float4((" + result + ").rgb, 1.0f)";
            const auto at = out.cg_fragment.rfind("return result;");
            out.cg_fragment.replace(at, std::string("return result;").size(), "return " + result + ";");
        }
        if (mesh && std::string(probe) == "vertex-white") {
            const auto at = out.cg_vertex.rfind("v_position =");
            out.cg_vertex.insert(at, "v_color0 = float4(1.0f, 1.0f, 1.0f, 1.0f);\n    ");
        }
    }
#endif
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
        return "geometry" + light + (key.material_gpu ? "_gpu" : "");
    std::string name = "mesh" + light + "_";
    for (const auto texgen : key.texgen)
        name += texgens[unsigned(texgen)];
    return name + (key.material_gpu ? "_gpu" : "");
}

std::string NativeShaderFragmentName(const NativeShaderFragmentKey& key) {
    return std::string(key.model == NativeShaderModel::Modulate ? "modulate" : "basic") + "_t" + std::to_string(key.textures) + (key.overlay ? "_overlay" : "") + (key.pure_modulate ? "_pure" : "") +
           (key.alpha_test ? "_alpha" : "") + (key.fog ? (key.linear_fog ? "_fog_linear" : "_fog") : "") + (key.target_alpha_one ? "_a1" : "") +
           (key.material_gpu ? (key.dynamic ? "_gpu_dyn" : "_gpu") : "");
}

std::string NativeShaderVertexName(NativeShaderVariant variant) {
    return NativeShaderVertexName(NativeShaderVertexKeyOf(variant));
}

std::string NativeShaderFragmentName(NativeShaderVariant variant) {
    return NativeShaderFragmentName(NativeShaderFragmentKeyOf(variant));
}
