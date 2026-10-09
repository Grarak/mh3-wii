// GXM runtime shader cache pack format (aurora-main/lib/gxm/shader_cache_pack.hpp).
#include "gxm/shader_cache_pack.hpp"
#include <cassert>
#include <cstdio>
#include <string>

using namespace aurora::gxm;

int main() {
  const std::string a("GXP\0abc", 7), b("GXP\0programB!", 13), c("GXP\0newer-a", 11);
  std::string pack = shader_pack::encode(1, 0, 10, a.data(), a.size());
  pack += shader_pack::encode(2, 1, 20, b.data(), b.size());
  assert(pack.size() == 32 + 8 + 32 + 16); // records padded to 4 bytes
  auto entries = shader_pack::parse(pack, 2, 1 << 20);
  assert(entries.size() == 2);
  assert(entries.at({1, 0}).first == 10 && entries.at({1, 0}).second == a);
  assert(entries.at({2, 1}).first == 20 && entries.at({2, 1}).second == b);

  // A later record replaces the program; the same hash with another stage is another program.
  pack += shader_pack::encode(1, 0, 11, c.data(), c.size());
  pack += shader_pack::encode(1, 2, 12, c.data(), c.size());
  entries = shader_pack::parse(pack, 2, 1 << 20);
  assert(entries.size() == 3 && entries.at({1, 0}).first == 11 && entries.at({1, 0}).second == c && entries.at({1, 2}).first == 12);

  // A truncated tail (interrupted append) and anything after a bad record are ignored.
  auto truncated = pack + shader_pack::encode(3, 0, 30, b.data(), b.size()).substr(0, 40);
  assert(shader_pack::parse(truncated, 2, 1 << 20).size() == 3);
  auto corrupt = shader_pack::encode(4, 9, 40, a.data(), a.size()) + pack; // stage out of range
  assert(shader_pack::parse(corrupt, 2, 1 << 20).empty());
  assert(shader_pack::parse(pack, 2, 4).empty()); // larger than the program limit
  assert(shader_pack::parse("", 2, 1 << 20).empty());
  std::puts("shader_cache_pack: ok");
  return 0;
}
