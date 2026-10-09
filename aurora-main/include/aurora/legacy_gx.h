#pragma once

#ifdef __cplusplus
extern "C" {
#define AURORA_GX_NORETURN [[noreturn]]
#else
#define AURORA_GX_NORETURN _Noreturn
#endif

typedef void (*AuroraLegacyGxPanicHandler)(const char* entry);
void aurora_set_legacy_gx_panic_handler(AuroraLegacyGxPanicHandler handler);
AURORA_GX_NORETURN void aurora_legacy_gx_panic(const char* entry);

#ifdef __cplusplus
}
#endif
#undef AURORA_GX_NORETURN

