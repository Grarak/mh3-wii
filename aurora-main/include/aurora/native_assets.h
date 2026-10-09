#pragma once
// Transport metadata for immutable assets converted during extraction.
// TEX0's reserved word carries a stable ID; the source Wii format remains
// available in the low bits so TEV and sampler state retain their meaning.
#define AURORA_NATIVE_TEXTURE_FORMAT_BIT 0x10000u
#define AURORA_NATIVE_TEXTURE_TAG 0xa0000000u

#define AURORA_GENERATED_TEXTURE_FORMAT_BIT 0x20000u
