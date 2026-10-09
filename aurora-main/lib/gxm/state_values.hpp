#pragma once
// State identifiers shared with Aurora's GLES frontend. No GLES API or driver.
#include <cstdint>
using GLenum = uint32_t;
using GLuint = uint32_t;
inline constexpr GLenum GL_ALWAYS = 0x0207;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_CONSTANT_ALPHA = 0x8003;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
inline constexpr GLenum GL_LINEAR_MIPMAP_NEAREST = 0x2701;
inline constexpr GLenum GL_MAX = 0x8008;
inline constexpr GLenum GL_MIRRORED_REPEAT = 0x8370;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_NEAREST_MIPMAP_LINEAR = 0x2702;
inline constexpr GLenum GL_NEAREST_MIPMAP_NEAREST = 0x2700;
inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ZERO = 0;
