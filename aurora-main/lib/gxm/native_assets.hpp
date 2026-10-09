#pragma once
#include "../gl/gl.hpp"
#include "../gfx/native_texture_pack.hpp"
#include <cstdint>
#include <vector>

namespace aurora::gxm {
class TextureStorage;
class NativeAssets {
public:
  ~NativeAssets();
  void open(const char* path);
  gl::NativeTextureInfo info(uint32_t id) const;
  void load(uint32_t id, TextureStorage& texture) const;
private:
  using Record = gfx::NativeTextureRecord;
  const Record& record(uint32_t id) const;
  int m_file = -1;
  std::vector<Record> m_records;
  uint32_t m_version = 0;
};
}
