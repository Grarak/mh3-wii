#pragma once

// 4x multisampling of the EFB, off by default: the Vita's GXM renderer
// resolves it into the EFB's own pixels; the other renderers render
// single-sampled. A launcher sets it before the game starts: the game's
// InitializeNativeGraphics applies it. The development MH3_MSAA overrides it.
bool MsaaSetting();
void SetMsaaSetting(bool msaa);
