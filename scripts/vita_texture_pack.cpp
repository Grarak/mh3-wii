// Offline only: preserve authored mips, resolve palettes, and repack CMPR
// blocks into SGX's Y-first Morton order. Nothing here runs on the Vita.
#include "gfx/texture_convert.hpp"
#include "gfx/native_texture_pack.hpp"
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
#include <atomic>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

// The encoder is an external offline dependency and never enters the game.
#ifdef STB_DXT_HEADER
#define STB_DXT_IMPLEMENTATION
#include STB_DXT_HEADER
#endif

namespace {
using Record=aurora::gfx::NativeTextureRecord;
struct Source {
  uint32_t id, offset, bytes, width, height, format, mips, palette, entries, paletteFormat;
  std::string path;
};
struct Prepared {Record record{};std::vector<uint8_t> payload;};
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
uint32_t rgba_word(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void write_color(uint8_t* out,uint32_t rgba){for(unsigned c=0;c<4;c++)out[c]=uint8_t(rgba>>(c*8));}
void exact_pixel(aurora::gl::Format f,const uint8_t* p,uint8_t* out){
  using F=aurora::gl::Format;
  if(f==F::RGBA8){std::memcpy(out,p,4);return;}
  const bool gray=p[0]==p[1]&&p[0]==p[2];
  if(f==F::U8_RRRR&&gray&&p[3]==p[0]){out[0]=p[0];return;}
  if(f==F::U8_1RRR&&gray&&p[3]==255){out[0]=p[0];return;}
  if(f==F::U8_R000&&p[0]==0&&p[1]==0&&p[2]==0){out[0]=p[3];return;}
  if(f==F::U8_R111&&p[0]==255&&p[1]==255&&p[2]==255){out[0]=p[3];return;}
  if(f==F::U8U8_GRRR&&gray){out[0]=p[0];out[1]=p[3];return;}
  if(f==F::RGB565&&p[3]==255&&((p[0]>>3)<<3|(p[0]>>3)>>2)==p[0]&&
      ((p[1]>>2)<<2|(p[1]>>2)>>4)==p[1]&&((p[2]>>3)<<3|(p[2]>>3)>>2)==p[2]){
    const uint16_t v=uint16_t((p[0]>>3)<<11|(p[1]>>2)<<5|p[2]>>3);out[0]=uint8_t(v);out[1]=uint8_t(v>>8);return;
  }
  throw std::runtime_error("Explicit exact texture policy changes a sampled RGBA channel");
}
std::vector<uint8_t> native_pixels(const Source& s,const uint8_t* rgba,aurora::gl::Format format){
  using namespace aurora;
  const auto layout=gfx::native_texture_layout(s.width,s.height,s.mips,format);
  std::vector<uint8_t> out(layout.total_bytes);
  const uint32_t paletteEntries=gfx::native_palette_entries(format),blockBytes=gfx::native_block_bytes(format);
  std::map<uint32_t,uint32_t> palette;
  if(paletteEntries){
    size_t texels=0;for(unsigned l=0;l<s.mips;l++)texels+=size_t(std::max(s.width>>l,1u))*std::max(s.height>>l,1u);
    for(size_t i=0;i<texels;i++){palette.try_emplace(rgba_word(rgba+i*4),0);if(palette.size()>paletteEntries)throw std::runtime_error("Explicit palette policy cannot represent all authored mips");}
    uint32_t index=0;for(auto& [color,value]:palette){value=index;write_color(out.data()+layout.palette_offset+index++*4,color);}
  }
  for(const auto& level:layout.levels){
    auto* destination=out.data()+level.offset;
    if(paletteEntries){
      for(uint32_t y=0;y<level.height;y++)for(uint32_t x=0;x<level.width;x++){
        const auto index=palette.at(rgba_word(rgba+(size_t(y)*level.width+x)*4));
        const auto at=morton(x,y,level.width,level.height);
        if(paletteEntries==16)destination[at/2]|=uint8_t(index<<(4*(at&1)));else destination[at]=uint8_t(index);
      }
    }else if(blockBytes){
      if(format!=gl::Format::BC4_RRRR&&format!=gl::Format::BC5_GRRR)throw std::runtime_error("Only explicit measured BC4/BC5 policies accept re-encoding");
      for(uint32_t y=0;y<level.height;y++)for(uint32_t x=0;x<level.width;x++){
        const auto* p=rgba+(size_t(y)*level.width+x)*4;
        if(p[0]!=p[1]||p[0]!=p[2]||(format==gl::Format::BC4_RRRR&&p[0]!=p[3]))
          throw std::runtime_error("Measured BC policy cannot reconstruct all source channels");
      }
#ifdef STB_DXT_HEADER
      const auto bw=std::max(level.width/4,1u),bh=std::max(level.height/4,1u);
      for(uint32_t y=0;y<bh;y++)for(uint32_t x=0;x<bw;x++){
        std::array<uint8_t,32> values{};
        for(uint32_t j=0;j<4;j++)for(uint32_t i=0;i<4;i++){
          const auto* p=rgba+(size_t(std::min(y*4+j,level.height-1))*level.width+std::min(x*4+i,level.width-1))*4;
          if(blockBytes==8)values[j*4+i]=p[0];else{values[(j*4+i)*2]=p[0];values[(j*4+i)*2+1]=p[3];}
        }
        auto* block=destination+morton(x,y,bw,bh)*blockBytes;
        if(blockBytes==8)stb_compress_bc4_block(block,values.data());else stb_compress_bc5_block(block,values.data());
      }
#else
      throw std::runtime_error("Measured BC policy requires the offline STB_DXT_HEADER build dependency");
#endif
    }else{
      const auto bytes=gfx::native_pixel_bytes(format);
      for(uint32_t y=0;y<level.height;y++)for(uint32_t x=0;x<level.width;x++)
        exact_pixel(format,rgba+(size_t(y)*level.width+x)*4,destination+size_t(y)*level.pitch+x*bytes);
    }
    rgba+=size_t(level.width)*level.height*4;
  }
  return out;
}
Prepared prepare(const Source& s,const std::vector<uint8_t>& file,uint32_t policy){
  if(uint64_t(s.offset)+s.bytes>file.size()||uint64_t(s.palette)+uint64_t(s.entries)*2>file.size())throw std::runtime_error("Source exceeds container");
  const aurora::ArrayRef<uint8_t> bytes{file.data()+s.offset,s.bytes};
  auto converted=s.entries?aurora::gfx::convert_texture_palette(s.format,s.width,s.height,s.mips,bytes,
      GXTlutFmt(s.paletteFormat),s.entries,{file.data()+s.palette,size_t(s.entries)*2}):
      aurora::gfx::convert_texture(s.format,s.width,s.height,s.mips,bytes);
  if(converted.data.empty())throw std::runtime_error("Texture conversion failed: "+s.path);
  Prepared prepared;auto& r=prepared.record;
  r.id=s.id;r.format=policy?policy:1;r.width=s.width;r.height=s.height;r.mips=s.mips;r.flags=converted.hasArbitraryMips?1:0;
  if(!policy&&s.format==GX_TF_CMPR&&std::has_single_bit(s.width)&&std::has_single_bit(s.height)){
    const bool alpha=has_transparency(s,bytes.data());r.format=alpha?3:2;prepared.payload=compressed(s,bytes.data(),alpha);
  }else prepared.payload=native_pixels(s,converted.data.data(),aurora::gfx::native_texture_format(r.format));
  const auto layout=aurora::gfx::native_texture_layout(s.width,s.height,s.mips,aurora::gfx::native_texture_format(r.format));
  if(prepared.payload.size()!=layout.total_bytes)throw std::runtime_error("Offline texture payload disagrees with native storage");
  r.bytes=prepared.payload.size();r.image_bytes=layout.image_bytes;r.palette_offset=layout.palette_offset;r.palette_bytes=layout.palette_bytes;
  r.source_format=s.format;r.source_palette_entries=s.entries;r.source_palette_format=s.entries?s.paletteFormat:0;
  const int64_t delta=int64_t(s.palette)-s.offset;
  if(s.entries&&(delta<INT32_MIN||delta>INT32_MAX))throw std::runtime_error("Original palette offset exceeds native record");
  r.source_palette_delta=s.entries?int32_t(delta):0;
  if(r.format==12||r.format==13)r.flags|=2;
  return prepared;
}
}
int main(int argc, char** argv) {
  try {
    if (argc < 3 || argc > 5) throw std::runtime_error("Usage: MH3VitaTexturePack manifest.tsv native-textures.bin [policy.tsv|-] [jobs]");
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
    if(sources.empty()||sources.size()>32768)throw std::runtime_error("Invalid native texture asset count");
    std::map<uint32_t,uint32_t> policy;
    if(argc>=4&&std::string(argv[3])!="-"){
      std::ifstream choices(argv[3]);if(!choices)throw std::runtime_error("Cannot open explicit texture policy");
      for(std::string line;std::getline(choices,line);){
        uint32_t id,format;std::string extra;std::istringstream fields(line);
        if(!(fields>>id>>format)||(fields>>extra))throw std::runtime_error("Invalid texture policy row");
        aurora::gfx::native_texture_format(format);
        if(!id||id>sources.size()||!policy.emplace(id,format).second)throw std::runtime_error("Invalid/duplicate explicit asset policy");
      }
    }
    std::map<std::string,std::vector<size_t>> grouped;
    for(size_t i=0;i<sources.size();i++){if(sources[i].id!=i+1)throw std::runtime_error("Texture IDs must match sorted manifest order");grouped[sources[i].path].push_back(i);}
    std::vector<std::pair<std::string,std::vector<size_t>>> groups(grouped.begin(),grouped.end());
    const unsigned workers=argc==5?unsigned(std::stoul(argv[4])):std::max(std::thread::hardware_concurrency(),1u);
    if(!workers||workers>1024)throw std::runtime_error("Invalid texture worker count");
    std::vector<Prepared> prepared(sources.size());std::atomic<size_t> next{0};std::atomic<bool> cancel{false};std::exception_ptr failure;std::mutex failureMutex;
    std::vector<std::thread> threads;
    for(unsigned worker=0;worker<std::min<size_t>(workers,groups.size());worker++)threads.emplace_back([&]{
      try{
        for(size_t i;(i=next.fetch_add(1))<groups.size()&&!cancel.load();){
          const auto& [path,indexes]=groups[i];std::ifstream input(path,std::ios::binary|std::ios::ate);
          if(!input)throw std::runtime_error("Cannot read "+path);
          const auto size=input.tellg();if(size<0)throw std::runtime_error("Cannot size "+path);
          std::vector<uint8_t> file(static_cast<size_t>(size));input.seekg(0);input.read(reinterpret_cast<char*>(file.data()),file.size());
          if(!input)throw std::runtime_error("Truncated container: "+path);
          for(const auto index:indexes){const auto found=policy.find(sources[index].id);prepared[index]=prepare(sources[index],file,found==policy.end()?0:found->second);}
        }
      }catch(...){std::lock_guard lock(failureMutex);if(!failure)failure=std::current_exception();cancel=true;}
    });
    for(auto& thread:threads)thread.join();if(failure)std::rethrow_exception(failure);
    std::fstream output(argv[2], std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create pack");
    const std::array<uint32_t, 8> header{0x5456334d, 2, uint32_t(sources.size()), sizeof(Record), 0, 0, 0, 0};
    const auto word=[&](uint32_t value){for(unsigned b=0;b<4;b++)output.put(char(value>>(8*b)));};
    for(auto value:header)word(value);
    std::vector<Record> records(sources.size());
    output.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(Record));
    uint64_t originalBytes = 0, nativeBytes = 0;
    std::array<unsigned, 14> counts{};
    for (size_t i = 0; i < sources.size(); ++i) {
      const auto& s = sources[i];
      auto r=prepared[i].record;auto& payload=prepared[i].payload;
      r.offset = (uint64_t(output.tellp()) + 63) & ~uint64_t(63);
      output.seekp(r.offset); output.write(reinterpret_cast<const char*>(payload.data()), payload.size());
      if (!output) throw std::runtime_error("Writing native payload failed");
      aurora::gfx::validate_native_texture_record(r,2,s.id,i?records[i-1].offset+records[i-1].bytes:32+records.size()*sizeof(Record),r.offset+r.bytes);
      records[i] = r; ++counts[r.format]; originalBytes += s.bytes; nativeBytes += r.bytes;
      std::vector<uint8_t>{}.swap(payload);
      if ((i + 1) % 1000 == 0) std::cout << i + 1 << " assets converted\n";
    }
    output.seekp(32);
    for(const auto& r:records){
      const uint32_t fields[]{r.id,r.format,r.width,r.height,r.mips,r.flags,r.bytes,r.palette_offset,
        uint32_t(r.offset),uint32_t(r.offset>>32),r.image_bytes,r.palette_bytes,std::bit_cast<uint32_t>(r.source_palette_delta),
        r.source_palette_entries,r.source_palette_format,r.source_format};
      for(auto value:fields)word(value);
    }
    if(!output)throw std::runtime_error("Writing native texture metadata failed");
    std::cout<<"Packed "<<records.size()<<" assets on "<<workers<<" workers; "<<nativeBytes<<" native / "<<originalBytes<<" Wii bytes\n";
    for(unsigned f=1;f<counts.size();f++)if(counts[f])std::cout<<"  format "<<f<<": "<<counts[f]<<" assets\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
