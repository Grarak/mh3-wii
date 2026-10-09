// Exercise the real allocator with bounded mock frame memory. No graphics
// context, payload copy or mirrored implementation is part of this fixture.
#include <aurora/native_frame_gate.hpp>
#include "../lib/gxm/native_capacity.hpp"
#include "../lib/gfx/native_geometry.cpp"
#include <array>
#include <cassert>
#include <cstdio>

namespace fixture {
alignas(256) std::array<uint8_t, 2048> vertices, uniforms, indices;
aurora::NativeFrameGate gate;
aurora::GpuFrameRing ring;
volatile unsigned completed = 0;
uint32_t serial = 1, frame = 0;
unsigned cancellations = 0, submitted = 0;
size_t vertexBytes = 2048, uniformBytes = 2048, indexBytes = 2048;
void begin(size_t v, size_t u, size_t i) {
  gate.end(); ++frame; ++serial;
  vertexBytes = v; uniformBytes = u; indexBytes = i;
  assert(gate.begin(true, true, [] { return ring.pending(); }));
}
}

namespace aurora::gfx {
uint32_t current_frame() noexcept { return fixture::frame; }
}

namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char*, const char*, unsigned int) noexcept { std::abort(); }
void Module::show_fatal_dialog(const char*, std::string_view) noexcept { std::abort(); }
NativeDrawMemory native_draw_memory() noexcept {
  if (!fixture::gate.should_render()) return {};
  return {{fixture::vertices.data(), fixture::vertexBytes},
          {fixture::uniforms.data(), fixture::uniformBytes},
          {fixture::indices.data(), fixture::indexBytes}, {}};
}
}

namespace aurora::gl {
bool on_render_thread() noexcept { return true; }
void draw_native_geometry(const AuroraNativeGeometry&, bool) noexcept { ++fixture::submitted; }
void draw_native_material_geometry(const AuroraNativeMaterialGeometry&, GLuint,
                                   const AuroraNativeUniforms&) noexcept { ++fixture::submitted; }
}

bool aurora_native_frame_should_render() { return fixture::gate.should_render(); }
uint32_t aurora_native_frame_slot() { return fixture::ring.slot(); }
void aurora_cancel_native_frame() {
  ++fixture::cancellations;

  // An earlier submitted scene remains attached to the current lease.
  fixture::ring.record_scene(&fixture::completed, fixture::serial);
  fixture::gate.cancel();
}

int main() {
  using namespace fixture;
  using namespace aurora::gxm;
  static_assert(NativeVertexBytes == 1048576 && NativeUniformBytes == 2097152);
  static_assert(NativeIndexBytes == 524288 && NativeSlotBytes == 3670016);
  static_assert(NativeProducerBytes == 11796480 && NativeProducerReserve == 12582912);
  assert(NativeVertexOffset + NativeVertexBytes <= NativeUniformOffset);
  assert(NativeUniformOffset + NativeUniformBytes <= NativeIndexOffset);
  assert(NativeIndexOffset + NativeIndexBytes == NativeSlotBytes);

  vertices.fill(0xa5); indices.fill(0xb6); uniforms.fill(0xc7);
  const auto vBefore = vertices, iBefore = indices, uBefore = uniforms;
  AuroraNativeGeometry a{}, b{};
  AuroraNativeMaterialGeometry material{};
  AuroraNativeUniforms u{}, next{};

  // Geometry/index ranges exactly fill; the next range cancels the frame.
  begin(32 + 28, 512, 8);
  assert(aurora_allocate_native_geometry(1, 2, &a));
  assert(aurora_allocate_native_geometry(1, 2, &b));
  assert(reinterpret_cast<uint8_t*>(a.vertices) == vertices.data());
  assert(reinterpret_cast<uint8_t*>(b.vertices) == vertices.data() + 32);
  assert(reinterpret_cast<uint8_t*>(b.indices) == indices.data() + 4);
  const auto before = aurora_native_draw_stats();
  const auto slot = ring.slot();
  assert(!aurora_allocate_native_geometry(1, 1, &b));
  assert(!b.vertices && !b.indices && !gate.should_render());
  assert(ring.slot() == slot && ring.pending());
  assert(!aurora_allocate_native_uniforms(16, &u));
  aurora_submit_native_geometry(&a, 0); // Latched suppression, no renderer call.
  assert(submitted == 0);
  completed = serial; // Completion cannot reopen this logical frame.
  assert(!aurora_allocate_native_geometry(1, 1, &b));
  assert(cancellations == 1 && ring.slot() == slot);
  const auto after = aurora_native_draw_stats();
  assert(after.vertex_high_water == before.vertex_high_water);
  assert(after.index_high_water == before.index_high_water);

  // A new logical frame resets cursors only after admission; uniforms align
  // to 256B and an exactly full final range succeeds.
  begin(288, 512, 24);
  assert(aurora_allocate_native_material_geometry(2, 12, &material));
  assert(material.vertices == reinterpret_cast<AuroraNativeMaterialVertex*>(vertices.data()));
  assert(aurora_allocate_native_uniforms(1, &u) && u.offset == 0);
  assert(aurora_allocate_native_uniforms(256, &next) && next.offset == 256);
  assert(!aurora_allocate_native_uniforms(1, &u));
  assert(!u.data && cancellations == 2);
  completed = serial;

  // Index overflow rejects both cursors atomically; the rejected call writes
  // no payload and the successful peak is not extended.
  begin(2048, 512, 1);
  assert(!aurora_allocate_native_geometry(1, 1, &a));
  assert(!a.vertices && !a.indices && cancellations == 3);
  completed = serial;
  begin(2048, 512, 2048);
  assert(!aurora_allocate_native_material_geometry(65537, 3, &material));
  assert(cancellations == 4);
  completed = serial;

  // Invalid/no-work calls do not cancel a valid frame. No API writes bytes;
  // callers fill only successful borrowed ranges.
  begin(2048, 512, 2048);
  assert(!aurora_allocate_native_geometry(1, 1, nullptr));
  assert(!aurora_allocate_native_geometry(0, 1, &a));
  assert(!aurora_allocate_native_uniforms(0, &u));
  assert(gate.should_render() && cancellations == 4);
  assert(vertices == vBefore && indices == iBefore && uniforms == uBefore);
  const auto high = aurora_native_draw_stats();
  assert(high.vertex_high_water == 288 && high.index_high_water == 24);
  assert(high.uniform_high_water == 512 && high.capacity_cancellations == 4);
  gate.end();
  std::puts("Native capacity: CDRAM slot layout, exact fills, aligned disjoint ranges, atomic rejection, latched whole-frame cancellation and protected leases passed");
}
