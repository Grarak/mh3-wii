#ifndef MH3_THP_VIDEO_H
#define MH3_THP_VIDEO_H

/* THP movie video frames: baseline JPEG with 4:2:0 sampling (Y 2x2, Cb and Cr
 * 1x1), whose entropy-coded data has no 0xFF00 byte stuffing: every byte after
 * SOS is scan data. The SDK's THPVideoDecode (0x804E17A0) writes Y, U (Cb)
 * and V (Cr) as GX I8 textures, 8x4-texel tiles of 32 bytes in row-major
 * order across each plane; this decoder writes the same planes.
 *
 * Every movie on the disc is 640x480 without restart intervals, so frames
 * with a DRI marker or dimensions that are not multiples of 16 are refused
 * (MH3_THP_UNSUPPORTED). The IDCT is the integer AAN one (libjpeg's ifast):
 * samples may differ from the SDK's paired-single IDCT by a unit or two.
 * Pure functions, no host APIs. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THPVideoDecode's results for the cases this decoder reports. */
enum {
    MH3_THP_OK = 0,
    MH3_THP_BAD_MARKER = 3,  /* the data does not continue with a marker */
    MH3_THP_BAD_PRECISION = 10,
    MH3_THP_UNSUPPORTED = 11,
    MH3_THP_BAD_COMPONENTS = 12,
    MH3_THP_BAD_TABLE = 15,  /* a scan refers to a table the frame lacks */
    MH3_THP_BAD_SAMPLING = 19,
    MH3_THP_BAD_DATA = 32,   /* truncated headers or an invalid Huffman code */
};

typedef struct {
    uint16_t fast[512];  /* 9-bit lookahead: (length << 8) | symbol, 0 = longer code */
    int32_t maxcode[17]; /* largest code of each length, -1 when the length has none */
    int32_t delta[17];   /* values index = code + delta[length] */
    uint8_t values[256];
} mh3_thp_huffman;

typedef struct {
    uint16_t width, height;
    uint8_t quant_table[3];  /* per component (Y, Cb, Cr) */
    uint8_t dc_table[3], ac_table[3];
    uint8_t tables_present;  /* bit n: quant table n; bit 4 + n: DC n; bit 6 + n: AC n */
    int32_t quant[2][64];    /* AAN-prescaled multipliers, natural order */
    mh3_thp_huffman dc[2], ac[2];
    const uint8_t *scan, *end;
} mh3_thp_frame;

/* Parses the markers up to SOS. `size` bounds every read, including the scan. */
int mh3_thp_parse(const uint8_t *data, size_t size, mh3_thp_frame *frame);
/* Decodes the scan into tiled I8 planes of width x height (Y) and
 * width/2 x height/2 (U, V). */
int mh3_thp_decode(const mh3_thp_frame *frame, uint8_t *y, uint8_t *u, uint8_t *v);

#ifdef __cplusplus
}
#endif

#endif
