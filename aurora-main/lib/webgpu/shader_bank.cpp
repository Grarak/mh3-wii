#include "shader_bank.hpp"
#include <aurora/fatal.hpp>
// ZSTD_createDDict_byReference; zstd is linked statically.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace aurora::webgpu::shader_bank {
namespace {
struct Decoder {
  ZSTD_DCtx* context = nullptr;
  ZSTD_DDict* dictionary = nullptr;
};

Decoder& decoder() {
  static Decoder value;

  if (!value.dictionary) {
    value.context = ZSTD_createDCtx();

    // The dictionary stays in the module's read-only data.
    value.dictionary = ZSTD_createDDict_byReference(Blob, DictionaryBytes);

    if (!value.context || !value.dictionary)
      AURORA_THROW(std::runtime_error("WebGPU shader bank: cannot create the zstd decoder"));
  }

  return value;
}

// The words after `tag` on its header line ("// tag a b c").
std::vector<std::string> header(const std::string& wgsl, const char* tag) {
  const std::string prefix = std::string("// ") + tag;
  const auto start = wgsl.find(prefix);
  std::vector<std::string> words;

  if (start == std::string::npos)
    AURORA_THROW(std::runtime_error(std::string("WebGPU shader bank: stage without ") + tag));

  std::istringstream line(wgsl.substr(start + prefix.size(), wgsl.find('\n', start) - start - prefix.size()));

  for (std::string word; line >> word;)
    words.push_back(word);

  return words;
}
} // namespace

bool find(uint64_t key, bool fragment, Module& out) {
  const auto* stages = fragment ? FragmentStages : VertexStages;
  const size_t count = fragment ? FragmentStageCount : VertexStageCount;
  const auto* found = std::lower_bound(stages, stages + count, key, [](const Stage& stage, uint64_t value) { return stage.key < value; });

  if (found == stages + count || found->key != key)
    return false;

  auto& state = decoder();
  out = {};
  out.wgsl.resize(found->size);
  const size_t written = ZSTD_decompress_usingDDict(state.context, out.wgsl.data(), found->size, Blob + found->offset, found->packed, state.dictionary);

  if (ZSTD_isError(written) || written != found->size)
    AURORA_THROW(std::runtime_error("WebGPU shader bank: corrupt stage"));

  for (const auto& block : header(out.wgsl, "mh3-blocks")) {
    const auto equals = block.find('=');
    out.blocks.emplace_back(block.substr(0, equals), uint32_t(std::stoul(block.substr(equals + 1))));
  }

  out.loose = header(out.wgsl, "mh3-loose");

  for (const auto& texture : header(out.wgsl, "mh3-textures")) {
    const auto first = texture.find(':'), second = texture.rfind(':');
    out.textures.push_back({texture.substr(0, first), texture[first + 1], texture.substr(second + 1) == "sample"});
  }

  return true;
}
} // namespace aurora::webgpu::shader_bank
