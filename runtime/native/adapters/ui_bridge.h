#pragma once
#include <cstdint>
#include "runtime/native/mh3_ui.h"
#include "runtime/native/mh3_model_state.h"
// Publish after a CPU font producer finishes replacing atlas pixels. The
// backend keys uploads by this generation; repeated glyph draws do not mutate it.
void NativeUIFontAtlasProduced(uint32_t payload_address);
using NativeUISourceObserver=void(*)(const mh3_ui_stream&,const mh3_draw_state&,
    const mh3_native_texture_binding&,const mh3_native_palette_source*);
// An explicitly installed CPU consumer observes authored source quads even
// during skipped frames. A null observer adds no source inspection to draws.
void SetNativeUISourceObserver(NativeUISourceObserver observer);
