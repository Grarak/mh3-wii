#include "mh3_thp_video.h"

#include <string.h>

/* Natural (row-major) index of each zigzag position. */
static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* AAN IDCT input scales: 1 for k = 0, else cos(k * pi / 16) * sqrt(2). */
static const double aan_scale[8] = {
    1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379,
};

static unsigned read16(const uint8_t *p) {
    return (unsigned)p[0] << 8 | p[1];
}

static int parse_quant(const uint8_t *p, size_t length, mh3_thp_frame *frame) {
    while (length) {
        const unsigned id = p[0] & 15;
        if (p[0] >> 4)
            return MH3_THP_UNSUPPORTED; /* 16-bit tables */
        if (id > 1)
            return MH3_THP_BAD_TABLE;
        if (length < 65)
            return MH3_THP_BAD_DATA;
        for (int k = 0; k < 64; ++k) {
            const int n = zigzag[k];
            /* Dequantized coefficients carry the ifast IDCT's 2 fraction bits. */
            frame->quant[id][n] = (int32_t)(p[1 + k] * aan_scale[n >> 3] * aan_scale[n & 7] * 4.0 + 0.5);
        }
        frame->tables_present |= 1u << id;
        p += 65;
        length -= 65;
    }
    return MH3_THP_OK;
}

static int parse_huffman(const uint8_t *p, size_t length, mh3_thp_frame *frame) {
    while (length) {
        if (length < 17)
            return MH3_THP_BAD_DATA;
        const unsigned table_class = p[0] >> 4, id = p[0] & 15;
        if (table_class > 1 || id > 1)
            return MH3_THP_BAD_TABLE;
        unsigned total = 0;
        for (int i = 1; i <= 16; ++i)
            total += p[i];
        if (total > 256 || length < 17 + total)
            return MH3_THP_BAD_DATA;
        mh3_thp_huffman *h = table_class ? &frame->ac[id] : &frame->dc[id];
        memset(h->fast, 0, sizeof(h->fast));
        memcpy(h->values, p + 17, total);
        int32_t code = 0;
        unsigned k = 0;
        h->maxcode[0] = h->delta[0] = -1;
        for (int bits = 1; bits <= 16; ++bits) {
            const unsigned count = p[bits];
            h->delta[bits] = (int32_t)k - code;
            for (unsigned i = 0; i < count; ++i, ++code, ++k) {
                if (bits <= 9) {
                    const unsigned first = (unsigned)code << (9 - bits);
                    for (unsigned j = 0; j < 1u << (9 - bits); ++j)
                        h->fast[first + j] = (uint16_t)(bits << 8 | h->values[k]);
                }
            }
            if (code > (1 << bits))
                return MH3_THP_BAD_DATA; /* over-subscribed lengths */
            h->maxcode[bits] = count ? code - 1 : -1;
            code <<= 1;
        }
        frame->tables_present |= 1u << ((table_class ? 6 : 4) + id);
        p += 17 + total;
        length -= 17 + total;
    }
    return MH3_THP_OK;
}

static int parse_frame(const uint8_t *p, size_t length, mh3_thp_frame *frame) {
    if (length < 6 + 9)
        return MH3_THP_BAD_DATA;
    if (p[0] != 8)
        return MH3_THP_BAD_PRECISION;
    frame->height = (uint16_t)read16(p + 1);
    frame->width = (uint16_t)read16(p + 3);
    if (p[5] != 3)
        return MH3_THP_BAD_COMPONENTS;
    for (int i = 0; i < 3; ++i) {
        const uint8_t *c = p + 6 + 3 * i;
        if (c[1] != (i ? 0x11 : 0x22))
            return MH3_THP_BAD_SAMPLING;
        if (c[2] > 1)
            return MH3_THP_BAD_TABLE;
        frame->quant_table[i] = c[2];
    }
    if (!frame->width || !frame->height || frame->width % 16 || frame->height % 16)
        return MH3_THP_UNSUPPORTED;
    return MH3_THP_OK;
}

static int parse_scan(const uint8_t *p, size_t length, mh3_thp_frame *frame) {
    if (length < 1 + 6 + 3)
        return MH3_THP_BAD_DATA;
    if (p[0] != 3)
        return MH3_THP_BAD_COMPONENTS;
    for (int i = 0; i < 3; ++i) {
        const unsigned dc = p[2 + 2 * i] >> 4, ac = p[2 + 2 * i] & 15;
        if (dc > 1 || ac > 1 || !(frame->tables_present & 1u << (4 + dc)) || !(frame->tables_present & 1u << (6 + ac)) ||
            !(frame->tables_present & 1u << frame->quant_table[i]))
            return MH3_THP_BAD_TABLE;
        frame->dc_table[i] = (uint8_t)dc;
        frame->ac_table[i] = (uint8_t)ac;
    }
    return MH3_THP_OK;
}

int mh3_thp_parse(const uint8_t *data, size_t size, mh3_thp_frame *frame) {
    memset(frame, 0, offsetof(mh3_thp_frame, quant));
    const uint8_t *p = data, *end = data + size;
    int have_frame = 0;
    for (;;) {
        if (p >= end)
            return MH3_THP_BAD_DATA;
        if (*p++ != 0xFF)
            return MH3_THP_BAD_MARKER;
        while (p < end && *p == 0xFF)
            ++p;
        if (p >= end)
            return MH3_THP_BAD_DATA;
        const uint8_t marker = *p++;
        if (marker == 0xD8)
            continue; /* SOI */
        if (end - p < 2)
            return MH3_THP_BAD_DATA;
        const size_t length = read16(p);
        if (length < 2 || (size_t)(end - p) < length)
            return MH3_THP_BAD_DATA;
        const uint8_t *segment = p + 2;
        const size_t segment_length = length - 2;
        p += length;
        int result = MH3_THP_OK;
        switch (marker) {
        case 0xDB:
            result = parse_quant(segment, segment_length, frame);
            break;
        case 0xC4:
            result = parse_huffman(segment, segment_length, frame);
            break;
        case 0xC0:
            result = parse_frame(segment, segment_length, frame);
            have_frame = 1;
            break;
        case 0xDA:
            if (!have_frame)
                return MH3_THP_BAD_DATA;
            result = parse_scan(segment, segment_length, frame);
            if (result == MH3_THP_OK) {
                frame->scan = p;
                frame->end = end;
            }
            return result;
        case 0xFE:
            break; /* comment */
        default:
            if (marker >= 0xE0 && marker <= 0xEF)
                break; /* application data */
            return MH3_THP_UNSUPPORTED; /* restart intervals, progressive frames, ... */
        }
        if (result != MH3_THP_OK)
            return result;
    }
}

/* Scan bits, MSB first: `buffer`'s top `bits` bits are the next ones. THP
 * scans are unstuffed, so every byte is data; past the end the scan reads 0. */
typedef struct {
    const uint8_t *p, *end;
    uint32_t buffer;
    int bits;
} bit_reader;

static inline void fill(bit_reader *b) {
    while (b->bits <= 24) {
        const uint32_t byte = b->p < b->end ? *b->p++ : 0;
        b->buffer |= byte << (24 - b->bits);
        b->bits += 8;
    }
}

static inline int decode_symbol(bit_reader *b, const mh3_thp_huffman *h) {
    fill(b);
    const unsigned entry = h->fast[b->buffer >> 23];
    if (entry) {
        const int length = (int)(entry >> 8);
        b->buffer <<= length;
        b->bits -= length;
        return (int)(entry & 0xFF);
    }
    for (int length = 10; length <= 16; ++length) {
        const int32_t code = (int32_t)(b->buffer >> (32 - length));
        if (code <= h->maxcode[length]) {
            b->buffer <<= length;
            b->bits -= length;
            return h->values[code + h->delta[length]];
        }
    }
    return -1;
}

/* The next `size` (1..16) bits as a signed coefficient (JPEG's EXTEND). */
static inline int receive_extend(bit_reader *b, int size) {
    if (b->bits < size)
        fill(b);
    const int value = (int)(b->buffer >> (32 - size));
    b->buffer <<= size;
    b->bits -= size;
    return value < 1 << (size - 1) ? value - (1 << size) + 1 : value;
}

/* Quantized coefficients in natural order; returns the last zigzag position
 * written, or -1 for invalid data. `coef` must be zero on entry. */
static inline int decode_block(bit_reader *b, const mh3_thp_huffman *dc, const mh3_thp_huffman *ac, int *prediction,
                               int16_t coef[64]) {
    const int dc_size = decode_symbol(b, dc);
    if (dc_size < 0 || dc_size > 11)
        return -1;
    if (dc_size)
        *prediction += receive_extend(b, dc_size);
    coef[0] = (int16_t)*prediction;
    int last = 0;
    for (int k = 1; k < 64;) {
        const int symbol = decode_symbol(b, ac);
        if (symbol < 0)
            return -1;
        const int run = symbol >> 4, size = symbol & 15;
        if (!size) {
            if (run != 15)
                break; /* end of block */
            k += 16;
            continue;
        }
        k += run;
        if (k > 63)
            return -1;
        coef[zigzag[k]] = (int16_t)receive_extend(b, size);
        last = k++;
    }
    return last;
}

static inline uint8_t clamp_sample(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

#define MULTIPLY(value, constant) (((value) * (constant)) >> 8)
#define FIX_1_082392200 277
#define FIX_1_414213562 362
#define FIX_1_847759065 473
#define FIX_2_613125930 669
/* Output descale: the ifast IDCT's 2 fraction bits and the 2-D transform's 8. */
#define OUTPUT_SHIFT 5
/* +128 level shift and rounding, added to each row's DC term. */
#define OUTPUT_BIAS ((128 << OUTPUT_SHIFT) + (1 << (OUTPUT_SHIFT - 1)))

/* AAN integer IDCT (libjpeg's jidctfst) of one block; rows 0-3 go to `top`,
 * rows 4-7 to `bottom`, 8 bytes per row (one I8 tile each). */
static void idct_store(const int16_t coef[64], int last, const int32_t quant[64], uint8_t *top, uint8_t *bottom) {
    if (!last) {
        const uint8_t value = clamp_sample((coef[0] * quant[0] + OUTPUT_BIAS) >> OUTPUT_SHIFT);
        memset(top, value, 32);
        memset(bottom, value, 32);
        return;
    }
    int32_t work[64];
    for (int column = 0; column < 8; ++column) {
        const int16_t *in = coef + column;
        const int32_t *q = quant + column;
        int32_t *w = work + column;
        if (!(in[8] | in[16] | in[24] | in[32] | in[40] | in[48] | in[56])) {
            const int32_t dc = in[0] * q[0];
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
            continue;
        }
        int32_t tmp0 = in[0] * q[0], tmp1 = in[16] * q[16], tmp2 = in[32] * q[32], tmp3 = in[48] * q[48];
        int32_t tmp10 = tmp0 + tmp2, tmp11 = tmp0 - tmp2, tmp13 = tmp1 + tmp3;
        int32_t tmp12 = MULTIPLY(tmp1 - tmp3, FIX_1_414213562) - tmp13;
        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;
        int32_t tmp4 = in[8] * q[8], tmp5 = in[24] * q[24], tmp6 = in[40] * q[40], tmp7 = in[56] * q[56];
        const int32_t z13 = tmp6 + tmp5, z10 = tmp6 - tmp5, z11 = tmp4 + tmp7, z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = MULTIPLY(z11 - z13, FIX_1_414213562);
        const int32_t z5 = MULTIPLY(z10 + z12, FIX_1_847759065);
        tmp10 = MULTIPLY(z12, FIX_1_082392200) - z5;
        tmp12 = MULTIPLY(z10, -FIX_2_613125930) + z5;
        tmp6 = tmp12 - tmp7;
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;
        w[0] = tmp0 + tmp7;
        w[56] = tmp0 - tmp7;
        w[8] = tmp1 + tmp6;
        w[48] = tmp1 - tmp6;
        w[16] = tmp2 + tmp5;
        w[40] = tmp2 - tmp5;
        w[32] = tmp3 + tmp4;
        w[24] = tmp3 - tmp4;
    }
    for (int row = 0; row < 8; ++row) {
        const int32_t *w = work + row * 8;
        uint8_t *out = row < 4 ? top + row * 8 : bottom + (row - 4) * 8;
        const int32_t dc = w[0] + OUTPUT_BIAS;
        if (!(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
            memset(out, clamp_sample(dc >> OUTPUT_SHIFT), 8);
            continue;
        }
        int32_t tmp10 = dc + w[4], tmp11 = dc - w[4], tmp13 = w[2] + w[6];
        int32_t tmp12 = MULTIPLY(w[2] - w[6], FIX_1_414213562) - tmp13;
        const int32_t tmp0 = tmp10 + tmp13, tmp3 = tmp10 - tmp13, tmp1 = tmp11 + tmp12, tmp2 = tmp11 - tmp12;
        const int32_t z13 = w[5] + w[3], z10 = w[5] - w[3], z11 = w[1] + w[7], z12 = w[1] - w[7];
        const int32_t tmp7 = z11 + z13;
        tmp11 = MULTIPLY(z11 - z13, FIX_1_414213562);
        const int32_t z5 = MULTIPLY(z10 + z12, FIX_1_847759065);
        tmp10 = MULTIPLY(z12, FIX_1_082392200) - z5;
        tmp12 = MULTIPLY(z10, -FIX_2_613125930) + z5;
        const int32_t tmp6 = tmp12 - tmp7, tmp5 = tmp11 - tmp6, tmp4 = tmp10 + tmp5;
        out[0] = clamp_sample((tmp0 + tmp7) >> OUTPUT_SHIFT);
        out[7] = clamp_sample((tmp0 - tmp7) >> OUTPUT_SHIFT);
        out[1] = clamp_sample((tmp1 + tmp6) >> OUTPUT_SHIFT);
        out[6] = clamp_sample((tmp1 - tmp6) >> OUTPUT_SHIFT);
        out[2] = clamp_sample((tmp2 + tmp5) >> OUTPUT_SHIFT);
        out[5] = clamp_sample((tmp2 - tmp5) >> OUTPUT_SHIFT);
        out[4] = clamp_sample((tmp3 + tmp4) >> OUTPUT_SHIFT);
        out[3] = clamp_sample((tmp3 - tmp4) >> OUTPUT_SHIFT);
    }
}

int mh3_thp_decode(const mh3_thp_frame *frame, uint8_t *y, uint8_t *u, uint8_t *v) {
    bit_reader b = {frame->scan, frame->end, 0, 0};
    int prediction[3] = {0, 0, 0};
    int16_t coef[64];
    memset(coef, 0, sizeof(coef));
    const unsigned luma_tiles = frame->width / 8u, chroma_tiles = frame->width / 16u;
    const size_t luma_band = (size_t)luma_tiles * 32, chroma_band = (size_t)chroma_tiles * 32;
    for (unsigned my = 0; my < frame->height / 16u; ++my) {
        for (unsigned mx = 0; mx < frame->width / 16u; ++mx) {
            for (int i = 0; i < 6; ++i) {
                const int component = i < 4 ? 0 : i - 3;
                const int last = decode_block(&b, &frame->dc[frame->dc_table[component]], &frame->ac[frame->ac_table[component]],
                                              &prediction[component], coef);
                if (last < 0)
                    return MH3_THP_BAD_DATA;
                /* An 8x8 block is one tile column and two 4-row tile bands. */
                uint8_t *top;
                size_t band;
                if (i < 4) {
                    band = luma_band;
                    top = y + (size_t)(my * 4 + (i >> 1) * 2) * luma_band + (size_t)(mx * 2 + (i & 1)) * 32;
                } else {
                    band = chroma_band;
                    top = (i == 4 ? u : v) + (size_t)(my * 2) * chroma_band + (size_t)mx * 32;
                }
                idct_store(coef, last, frame->quant[frame->quant_table[component]], top, top + band);
                if (last)
                    memset(coef, 0, sizeof(coef));
                else
                    coef[0] = 0;
            }
        }
    }
    return MH3_THP_OK;
}
