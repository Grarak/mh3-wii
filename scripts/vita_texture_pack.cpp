// Offline only: preserve authored mips, resolve palettes, and repack CMPR
// blocks into SGX's Y-first Morton order. Nothing here runs on the Vita.
#include "gfx/texture_convert.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Record {
  uint32_t id, format, width, height, mips, flags, bytes, reserved;
  uint64_t offset;
};
static_assert(sizeof(Record) == 40);
struct Source {
  uint32_t id, offset, bytes, width, height, format, mips, palette, entries, paletteFormat;
  std::string path;
};
uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint8_t reverse_pairs(uint8_t value) {
  return (value >> 6) | ((value >> 2) & 12) | ((value << 2) & 48) | (value << 6);
}
size_t morton(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  size_t index = 0;
  unsigned shift = 0;
  for (unsigned bit = 0; (1u << bit) < std::max(width, height); ++bit) {
    if ((1u << bit) < height) index |= size_t((y >> bit) & 1) << shift++;
    if ((1u << bit) < width) index |= size_t((x >> bit) & 1) << shift++;
  }
  return index;
}
bool has_transparency(const Source& s, const uint8_t* data) {
  uint32_t w = s.width, h = s.height;
  for (unsigned mip = 0; mip < s.mips; ++mip) {
    for (unsigned yy = 0; yy < h; yy += 8)
      for (unsigned xx = 0; xx < w; xx += 8)
        for (unsigned yb = 0; yb < 8; yb += 4)
          for (unsigned xb = 0; xb < 8; xb += 4, data += 8) {
            if (be16(data) > be16(data + 2)) continue;
            for (unsigned y = 0; y < 4 && yy + yb + y < h; ++y)
              for (unsigned x = 0; x < 4 && xx + xb + x < w; ++x)
                if (((data[4 + y] >> (6 - 2 * x)) & 3) == 3) return true;
          }
    w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
  }
  return false;
}
std::vector<uint8_t> compressed(const Source& s, const uint8_t* data, bool alpha) {
  std::vector<uint8_t> output;
  const unsigned blockBytes = alpha ? 16 : 8;
  uint32_t w = s.width, h = s.height;
  for (unsigned mip = 0; mip < s.mips; ++mip) {
    const uint32_t bw = std::max(w / 4, 1u), bh = std::max(h / 4, 1u);
    const size_t start = output.size();
    output.resize(start + size_t(bw) * bh * blockBytes);
    for (unsigned yy = 0; yy < h; yy += 8)
      for (unsigned xx = 0; xx < w; xx += 8)
        for (unsigned yb = 0; yb < 8; yb += 4)
          for (unsigned xb = 0; xb < 8; xb += 4, data += 8) {
            if (xx + xb >= w || yy + yb >= h) continue;
            auto* dest = output.data() + start + morton((xx + xb) / 4, (yy + yb) / 4, bw, bh) * blockBytes;
            const bool threeColor = be16(data) <= be16(data + 2);
            if (alpha) {
              // BC3 keeps CMPR's transparent texels' midpoint RGB. SGX uses
              // the three-colour endpoint rule for BC3 too (hardware guide).
              uint64_t alphaBits = 0;
              for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                  if (threeColor && ((data[4 + y] >> (6 - 2 * x)) & 3) == 3)
                    alphaBits |= uint64_t(1) << (3 * (4 * y + x));
              dest[0] = 255; dest[1] = 0;
              for (unsigned i = 0; i < 6; ++i) dest[2 + i] = alphaBits >> (8 * i);
              dest += 8;
            }
            dest[0] = data[1]; dest[1] = data[0]; dest[2] = data[3]; dest[3] = data[2];
            for (unsigned y = 0; y < 4; ++y) {
              uint8_t row = data[4 + y];
              if (alpha && threeColor)
                for (unsigned x = 0; x < 4; ++x) {
                  const unsigned shift = 6 - x * 2;
                  if (((row >> shift) & 3) == 3) row &= ~(1u << shift);
                }
              dest[4 + y] = reverse_pairs(row);
            }
          }
    w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
  }
  return output;
}
std::vector<uint8_t> linear(const Source& s, const uint8_t* data) {
  std::vector<uint8_t> output;
  for (unsigned mip = 0; mip < s.mips; ++mip) {
    const auto w = std::max(s.width >> mip, 1u), h = std::max(s.height >> mip, 1u);
    const auto pitch = (w + 7) & ~7u;
    const auto rows = mip + 1 < s.mips ? std::max(std::bit_ceil(s.height) >> mip, 1u) : h;
    const auto storedWidth = mip + 1 < s.mips ? std::max(std::bit_ceil(s.width) >> mip, 8u) : pitch;
    const auto offset = output.size();
    output.resize(offset + size_t(storedWidth) * rows * 4);
    for (unsigned y = 0; y < h; ++y) std::memcpy(output.data() + offset + size_t(y) * pitch * 4, data + size_t(y) * w * 4, size_t(w) * 4);
    data += size_t(w) * h * 4;
  }
  return output;
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("Usage: MH3VitaTexturePack manifest.tsv native-textures.bin");
    std::ifstream manifest(argv[1]);
    if (!manifest) throw std::runtime_error("Cannot open manifest");
    std::vector<Source> sources;
    for (std::string line; std::getline(manifest, line);) {
      std::istringstream fields(line);
      Source s{};
      if (!(fields >> s.id >> s.offset >> s.bytes >> s.width >> s.height >> s.format >> s.mips >> s.palette >> s.entries >> s.paletteFormat))
        throw std::runtime_error("Invalid manifest row");
      fields.get(); std::getline(fields, s.path);
      sources.push_back(std::move(s));
    }
    std::fstream output(argv[2], std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create pack");
    const std::array<uint32_t, 8> header{0x5456334d, 1, uint32_t(sources.size()), sizeof(Record), 0, 0, 0, 0};
    output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    std::vector<Record> records(sources.size());
    output.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(Record));
    std::string path;
    std::vector<uint8_t> file;
    uint64_t originalBytes = 0, nativeBytes = 0;
    std::array<unsigned, 4> counts{};
    for (size_t i = 0; i < sources.size(); ++i) {
      const auto& s = sources[i];
      if (s.path != path) {
        path = s.path;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot read " + path);
        file.resize(size_t(input.tellg())); input.seekg(0);
        input.read(reinterpret_cast<char*>(file.data()), file.size());
      }
      if (uint64_t(s.offset) + s.bytes > file.size() || uint64_t(s.palette) + s.entries * 2 > file.size())
        throw std::runtime_error("Source exceeds container");
      const aurora::ArrayRef<uint8_t> bytes{file.data() + s.offset, s.bytes};
      auto converted = s.entries
          ? aurora::gfx::convert_texture_palette(s.format, s.width, s.height, s.mips, bytes,
                GXTlutFmt(s.paletteFormat), s.entries, {file.data() + s.palette, size_t(s.entries) * 2})
          : aurora::gfx::convert_texture(s.format, s.width, s.height, s.mips, bytes);
      if (converted.data.empty()) throw std::runtime_error("Texture conversion failed: " + path);
      Record r{s.id, 1, s.width, s.height, s.mips, converted.hasArbitraryMips ? 1u : 0u, 0, 0, 0};
      std::vector<uint8_t> payload;
      if (s.format == GX_TF_CMPR && std::has_single_bit(s.width) && std::has_single_bit(s.height)) {
        const bool alpha = has_transparency(s, bytes.data());
        r.format = alpha ? 3 : 2;
        payload = compressed(s, bytes.data(), alpha);
      } else payload = linear(s, converted.data.data());
      r.offset = (uint64_t(output.tellp()) + 63) & ~uint64_t(63);
      r.bytes = payload.size();
      output.seekp(r.offset); output.write(reinterpret_cast<const char*>(payload.data()), payload.size());
      if (!output) throw std::runtime_error("Writing native payload failed");
      records[i] = r; ++counts[r.format]; originalBytes += s.bytes; nativeBytes += r.bytes;
      if ((i + 1) % 1000 == 0) std::cout << i + 1 << " assets converted\n";
    }
    output.seekp(32); output.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(Record));
    std::cout << "Packed " << records.size() << " assets: RGBA8 " << counts[1] << ", BC1 " << counts[2]
              << ", BC3 " << counts[3] << "; " << nativeBytes << " native / " << originalBytes << " Wii bytes\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
