#pragma once
#include <cstdint>
// Publish after a CPU font producer finishes replacing atlas pixels. The
// backend keys uploads by this generation; repeated glyph draws do not mutate it.
void NativeUIFontAtlasProduced(uint32_t payload_address);
