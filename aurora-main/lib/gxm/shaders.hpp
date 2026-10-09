#pragma once
#include <cstddef>
#include <cstdint>

namespace aurora::gxm {
struct ShaderBinary {
  uint64_t sourceHash;
  const unsigned char* bytes;
  size_t size;
};
extern const ShaderBinary VertexShaders[];
extern const ShaderBinary FragmentShaders[];
extern const ShaderBinary FragmentScalarShaders[];
extern const size_t VertexShaderCount;
extern const size_t FragmentShaderCount;
extern const size_t FragmentScalarShaderCount;
}
