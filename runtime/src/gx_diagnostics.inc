// Included only with MH3_ENABLE_GX_DIAGNOSTICS. Control file values:
// 0 normal, 1 disable GX fog, 2 omit orthographic draws, 3 trace one frame.
int gxDiagnosticMode = 0;
bool gxDiagnosticTrace = false;
unsigned gxDiagnosticDraw = 0;
void UpdateGxDiagnosticMode() {
    gxDiagnosticTrace = false;
    gxDiagnosticDraw = 0;
    if (const char* path = std::getenv("MH3_GX_DEBUG_CONTROL")) {
        int mode = 0;
        std::ifstream(path) >> mode;
        if (mode < 0 || mode > 3) mode = 0;
        if (mode != gxDiagnosticMode) {
            std::fprintf(stderr, "MH3 GX diagnostic mode %d at copy %llu\n", mode,
                         static_cast<unsigned long long>(guestCopies));
            gxDiagnosticTrace = mode == 3;
            gxDiagnosticMode = mode;
        }
    }
}
struct GxDiagnosticDraw {
    bool skip = false, changed = false;
    GXFogType saved = GX_FOG_NONE;
    explicit GxDiagnosticDraw(bool draw) {
        if (!draw) return;
        auto& state = aurora::gx::g_gxState;
        if (gxDiagnosticTrace && (state.projType == GX_ORTHOGRAPHIC || gxDiagnosticDraw < 10)) {
            const auto& v = state.logicalViewport;
            const auto& s = state.logicalScissor;
            std::fprintf(stderr, "MH3 GX state draw=%u projection=%u fog=%u viewport=%.1f,%.1f,%.1f,%.1f scissor=%d,%d,%d,%d blend=%u,%u,%u tev=%u texgen=%u\n",
                gxDiagnosticDraw, unsigned(state.projType), unsigned(state.fog.type),
                v.left, v.top, v.width, v.height, s.x, s.y, s.width, s.height,
                unsigned(state.blendMode), unsigned(state.blendFacSrc), unsigned(state.blendFacDst),
                state.numTevStages, state.numTexGens);
        }
        ++gxDiagnosticDraw;
        skip = gxDiagnosticMode == 2 && state.projType == GX_ORTHOGRAPHIC;
        if (gxDiagnosticMode == 1 && state.fog.type != GX_FOG_NONE) {
            saved = state.fog.type;
            state.fog.type = GX_FOG_NONE;
            state.stateDirty = true;
            ++state.pipelineStateGeneration;
            changed = true;
        }
    }
    ~GxDiagnosticDraw() {
        if (!changed) return;
        auto& state = aurora::gx::g_gxState;
        state.fog.type = saved;
        state.stateDirty = true;
        ++state.pipelineStateGeneration;
    }
};
