#include "shader_bank.hpp"
// ZSTD_createDDict_byReference; zstd is linked statically.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace aurora::gxm::shader_bank {
namespace {
struct Table {
  const Program* programs;
  size_t count;
};

Table table(ShaderStage stage) noexcept {
  switch (stage) {
  case ShaderStage::Vertex:
    return {VertexPrograms, VertexProgramCount};
  case ShaderStage::Fragment:
    return {FragmentPrograms, FragmentProgramCount};
  case ShaderStage::ScalarFragment:
    return {FragmentScalarPrograms, FragmentScalarProgramCount};
  }

  return {nullptr, 0};
}

const Program* lookup(uint64_t key, ShaderStage stage) noexcept {
  const auto [programs, count] = table(stage);
  const auto* found = std::lower_bound(programs, programs + count, key, [](const Program& program, uint64_t value) { return program.key < value; });
  return found != programs + count && found->key == key ? found : nullptr;
}

// Decompressed programs by table position; never freed.
struct Loaded {
  std::unique_ptr<const SceGxmProgram*[]> programs[3];
  ZSTD_DCtx* context = nullptr;
  ZSTD_DDict* dictionary = nullptr;
};

Loaded& loaded() {
  static Loaded value;

  if (!value.dictionary) {
    value.context = ZSTD_createDCtx();

    // The dictionary stays in the executable's read-only data.
    value.dictionary = ZSTD_createDDict_byReference(Blob, DictionaryBytes);

    if (!value.context || !value.dictionary)
      throw std::runtime_error("GXM shader bank: cannot create the zstd decoder");
  }

  return value;
}

std::string describe(uint64_t key, ShaderStage stage) {
  char text[24];
  std::snprintf(text, sizeof text, "%016llx_%c", static_cast<unsigned long long>(key), "vfr"[unsigned(stage)]);
  return text;
}

// Decompresses one program into `bytes` (entry.size) and validates it.
void unpack(const Program& entry, ShaderStage stage, void* bytes) {
  auto& state = loaded();
  const size_t written = ZSTD_decompress_usingDDict(state.context, bytes, entry.size, Blob + entry.offset, entry.packed, state.dictionary);
  const auto* program = static_cast<const SceGxmProgram*>(bytes);
  const auto type = stage == ShaderStage::Vertex ? SCE_GXM_VERTEX_PROGRAM : SCE_GXM_FRAGMENT_PROGRAM;

  if (ZSTD_isError(written) || written != entry.size || sceGxmProgramCheck(program) < 0 || sceGxmProgramGetSize(program) > entry.size || sceGxmProgramGetType(program) != type)
    throw std::runtime_error("GXM shader bank: corrupt program " + describe(entry.key, stage));
}
} // namespace

bool contains(uint64_t key, ShaderStage stage) noexcept {
  return lookup(key, stage) != nullptr;
}

const SceGxmProgram* find(uint64_t key, ShaderStage stage) {
  const Program* entry = lookup(key, stage);

  if (!entry)
    return nullptr;

  auto& slots = loaded().programs[unsigned(stage)];
  const auto [programs, count] = table(stage);

  if (!slots)
    slots = std::make_unique<const SceGxmProgram*[]>(count);

  auto& slot = slots[size_t(entry - programs)];

  if (slot)
    return slot;

  void* bytes = memalign(64, entry->size);

  if (!bytes)
    throw std::bad_alloc();

  try {
    unpack(*entry, stage, bytes);
  } catch (...) {
    std::free(bytes);
    throw;
  }

  slot = static_cast<const SceGxmProgram*>(bytes);
  return slot;
}

size_t check_all() {
  size_t checked = 0, largest = 0;

  for (const auto stage : {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::ScalarFragment}) {
    const auto [programs, count] = table(stage);

    for (size_t i = 0; i < count; ++i)
      largest = std::max<size_t>(largest, programs[i].size);
  }

  std::unique_ptr<void, decltype(&std::free)> scratch(memalign(64, largest), std::free);

  if (!scratch)
    throw std::bad_alloc();

  for (const auto stage : {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::ScalarFragment}) {
    const auto [programs, count] = table(stage);

    for (size_t i = 0; i < count; ++i, ++checked)
      unpack(programs[i], stage, scratch.get());
  }

  return checked;
}
} // namespace aurora::gxm::shader_bank
