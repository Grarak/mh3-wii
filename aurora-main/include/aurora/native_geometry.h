#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Homogeneous GL clip coordinates, with Y up. Backends account for surface
// orientation. This layout is fetched as attributes by GXM, without word loads.
typedef struct AuroraNativeVertex {
  float position[4];
  float uv[2];
  uint8_t color[4];
} AuroraNativeVertex;

typedef struct AuroraNativeGeometry {
  AuroraNativeVertex* vertices;
  uint16_t* indices;
  uint32_t vertex_count, index_count;
  uint32_t frame, slot;
} AuroraNativeGeometry;

// Dynamic effects may have independent projective texture coordinates and
// lighting/fog inputs. Shader reflection fetches only the attributes used by
// the selected material. Static model meshes keep their compact asset layout.
typedef struct AuroraNativeMaterialVertex {
  float position[4], view_position[3], normal[3], texcoord[8][3];
  uint8_t color[2][4];
} AuroraNativeMaterialVertex;
typedef struct AuroraNativeMaterialGeometry {
  AuroraNativeMaterialVertex* vertices;
  uint16_t* indices;
  uint32_t vertex_count, index_count;
  uint32_t frame, slot;
} AuroraNativeMaterialGeometry;
typedef struct AuroraNativeUniforms {
  void* data;
  uint32_t offset, size, frame, slot;
} AuroraNativeUniforms;

// Cumulative successful cursor peaks, including vertex/uniform alignment.
// Query on the graphics owner. Capacity rejection cancels the presentation;
// it never wraps a cursor or reopens drawing in the same logical frame.
typedef struct AuroraNativeDrawStats {
  uint64_t vertex_high_water, uniform_high_water, index_high_water;
  uint64_t capacity_cancellations;
} AuroraNativeDrawStats;
AuroraNativeDrawStats aurora_native_draw_stats(void);

// Borrow a disjoint range from the admitted frame's existing draw memory.
// Returns false on a skipped frame or exhausted space. An admitted frame that
// exhausts space is canceled as a whole. Never waits or wraps.
// Fill before submitting and do not retain a writable view after frame end.
int aurora_allocate_native_geometry(uint32_t vertices, uint32_t indices, AuroraNativeGeometry* out);
int aurora_allocate_native_material_geometry(uint32_t vertices, uint32_t indices, AuroraNativeMaterialGeometry* out);
int aurora_allocate_native_uniforms(uint32_t bytes, AuroraNativeUniforms* out);
void aurora_submit_native_material_geometry(const AuroraNativeMaterialGeometry* geometry, uint32_t program, const AuroraNativeUniforms* uniforms);
// Submit using the backend's current target, viewport, raster and sampler 0.
// texture_modulate selects sampled RGBA * vertex RGBA; zero selects vertex RGBA.
void aurora_submit_native_geometry(const AuroraNativeGeometry* geometry, int texture_modulate);
#ifdef __cplusplus
}
#endif
