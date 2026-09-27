#!/usr/bin/env python3
"""Apply fail-closed overlays to build-local Aurora sources only."""
import sys
from pathlib import Path
kind, filename = sys.argv[1:]
p = Path(filename)
s = p.read_text()
def replace(old, new, count=1):
    global s
    if s.count(old) != count:
        raise SystemExit(f'Shared-buffer overlay no longer matches {kind}: {old[:100]}')
    s = s.replace(old, new)

if kind == 'gpu':
    replace("""    const wgpu::RequestAdapterOptions options{
        .powerPreference""", """    // Dawn gates this native mapping feature as experimental. Opt in at
    // adapter creation, then still require the advertised Metal capability.
    const char* mh3_nativeFeatures[] = {"allow_unsafe_apis"};
    const wgpu::DawnTogglesDescriptor mh3_adapterToggles({
      .enabledToggleCount = backend == wgpu::BackendType::Metal ? 1u : 0u,
      .enabledToggles = mh3_nativeFeatures,
    });
    const wgpu::RequestAdapterOptions options{
        .nextInChain = &mh3_adapterToggles,
        .powerPreference""")
    replace('''      const auto feature = supportedFeatures.features[i];''', '''      const auto feature = supportedFeatures.features[i];
      // Metal UMA permits CPU-mappable buffers to be used directly by draws.
      if (g_backendType == wgpu::BackendType::Metal &&
          feature == wgpu::FeatureName::BufferMapExtendedUsages) {
        requiredFeatures.push_back(feature);
      }''')
    replace('''    enableToggles.push_back("skip_validation");''', '''#ifndef MH3_ENABLE_GPU_VALIDATION
    enableToggles.push_back("skip_validation");
#endif''')
elif kind == 'common':
    replace('''void initialize() {
  g_frameIndex = 0;''', '''// Directly bind the mapped staging ring after Unmap. The same MapAsync
// lifecycle used by the upload path now also fences prior shader/index reads.
static bool mh3_sharedRenderBuffers = false;
uint64_t mh3_uniform_buffer_offset() { return mh3_sharedRenderBuffers ? VertexBufferSize : 0; }
uint64_t mh3_index_buffer_offset() { return mh3_sharedRenderBuffers ? VertexBufferSize + UniformBufferSize : 0; }
static void mh3_bind_shared_slot();
void initialize() {
  const char* shared = std::getenv("MH3_SHARED_RENDER_BUFFERS");
  mh3_sharedRenderBuffers = webgpu::g_backendType == wgpu::BackendType::Metal &&
      g_device.HasFeature(wgpu::FeatureName::BufferMapExtendedUsages) &&
      (!shared || std::strcmp(shared, "0") != 0);
  Log.info("MH3 direct shared render buffers: {}", mh3_sharedRenderBuffers ? "enabled" : "disabled");
  g_frameIndex = 0;''')
    replace('''  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform''', '''  if (!mh3_sharedRenderBuffers) {
  createBuffer(g_uniformBuffer, wgpu::BufferUsage::Uniform''')
    replace('''               "Shared Storage Buffer");
  for''', '''               "Shared Storage Buffer");
  }
  for''')
    replace('''wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc, StagingBufferSize,''', '''wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc |
        (mh3_sharedRenderBuffers ? wgpu::BufferUsage::Storage | wgpu::BufferUsage::Uniform |
          wgpu::BufferUsage::Index : wgpu::BufferUsage::None), StagingBufferSize,''')
    replace('''  currentStagingBuffer = 0;
  s_mappingState.reset();''', '''  if (mh3_sharedRenderBuffers) {
    g_vertexBuffer = g_uniformBuffer = g_indexBuffer = g_storageBuffer = g_stagingBuffers[0];
  }
  currentStagingBuffer = 0;
  s_mappingState.reset();''')
    replace('''            .buffer = g_vertexBuffer,
''', '''            .buffer = g_vertexBuffer,
            .size = VertexBufferSize,
''')
    replace('''            .buffer = g_storageBuffer,
''', '''            .buffer = g_storageBuffer,
            .offset = mh3_sharedRenderBuffers ? VertexBufferSize + UniformBufferSize + IndexBufferSize : 0,
            .size = StorageBufferSize,
''')
    replace('''            .buffer = g_uniformBuffer,
''', '''            .buffer = g_uniformBuffer,
            .offset = mh3_uniform_buffer_offset(),
''')
    # Layouts exist by the time the first frame begins. Bind groups retain each
    # slot's buffer; globals only switch at begin, after the encode worker joins.
    anchor='void map_staging_buffer() {'
    replace(anchor, '''static void mh3_bind_shared_slot() {
  if (!mh3_sharedRenderBuffers) return;
  g_vertexBuffer = g_uniformBuffer = g_indexBuffer = g_storageBuffer = g_stagingBuffers[currentStagingBuffer];
  const std::array entries{
    wgpu::BindGroupEntry{.binding = 0, .buffer = g_vertexBuffer, .size = VertexBufferSize},
    wgpu::BindGroupEntry{.binding = 1, .buffer = g_storageBuffer,
      .offset = VertexBufferSize + UniformBufferSize + IndexBufferSize, .size = StorageBufferSize},
  };
  const wgpu::BindGroupDescriptor storage{.label = "MH3 shared vertex/storage slot",
    .layout = g_staticBindGroupLayout, .entryCount = entries.size(), .entries = entries.data()};
  g_staticBindGroup = g_device.CreateBindGroup(&storage);
  const wgpu::BindGroupEntry entry{.binding = 0, .buffer = g_uniformBuffer,
    .offset = VertexBufferSize, .size = gx::MaxUniformSize};
  const wgpu::BindGroupDescriptor uniform{.label = "MH3 shared uniform slot",
    .layout = g_uniformBindGroupLayout, .entryCount = 1, .entries = &entry};
  g_uniformBindGroup = g_device.CreateBindGroup(&uniform);
}

''' + anchor)
    replace('''  ++g_stagingEpoch;
  g_recordingSnapshotSlot''', '''  mh3_bind_shared_slot();
  ++g_stagingEpoch;
  g_recordingSnapshotSlot''')
    replace('''      cmd.CopyBufferToBuffer(g_stagingBuffers[currentStagingBuffer], bufferOffset, out, 0, AURORA_ALIGN(writeSize, 4));''', '''      if (!mh3_sharedRenderBuffers)
        cmd.CopyBufferToBuffer(g_stagingBuffers[currentStagingBuffer], bufferOffset, out, 0, AURORA_ALIGN(writeSize, 4));''')
elif kind == 'pipeline':
    replace('namespace aurora::gx {', '''namespace aurora::gfx { uint64_t mh3_index_buffer_offset(); }
namespace aurora::gx {''')
    replace('''pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);''', '''pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16,
        gfx::mh3_index_buffer_offset(), gfx::IndexBufferSize);''')
elif kind == 'tex_copy':
    replace('namespace aurora::gfx::tex_copy_conv {', '''namespace aurora::gfx { uint64_t mh3_uniform_buffer_offset(); }
namespace aurora::gfx::tex_copy_conv {''')
    replace('''.offset = req.uniformRange.offset,''', '''.offset = req.uniformRange.offset + mh3_uniform_buffer_offset(),''', 2)
else:
    raise SystemExit(f'Unknown overlay {kind}')
p.write_text(s)
