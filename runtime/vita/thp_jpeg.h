#pragma once

#include <cstddef>
#include <cstdint>

// THP movie frames on the Vita's hardware JPEG decoder (SceJpeg on the media
// engine) instead of the native software decoder (mh3_thp_video.h): about
// 26 ms of the game thread per 640x480 frame on the Cortex-A9.

// A movie frame read ahead of the one being decoded: its THP data.
struct VitaThpFrame {
    const uint8_t* data;
    size_t bytes;
};

// Decodes the THP frame `data` (`bytes` long: the video size from its frame
// header) into the same tiled I8 planes as THPVideoDecode: Y of width x
// height and U, V of half that. `ahead` lists the frames the movie reader
// has read after it, in order: the hardware decodes them while the game
// runs, so their own calls only tile. False when the hardware decoder is
// off, unavailable or refuses the frame: the caller decodes it in software.
bool VitaThpDecode(const uint8_t* data, size_t bytes, uint32_t width, uint32_t height, uint8_t* y, uint8_t* u, uint8_t* v, const VitaThpFrame* ahead,
                   size_t aheadCount);
