#include "gfx/native_texture_archive.hpp"
#include "gfx/native_texture_decode.hpp"
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::vector<uint8_t> read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open texture reference");return {std::istreambuf_iterator<char>(file),{}};}
void invalid_records(){
  using namespace aurora::gfx;
  const auto layout=native_texture_layout(16,8,5,aurora::gl::Format::P4_ABGR);
  NativeTextureRecord r{1,4,16,8,5,0,uint32_t(layout.total_bytes),uint32_t(layout.palette_offset),128,
      uint32_t(layout.image_bytes),uint32_t(layout.palette_bytes),0,0,0,6};
  validate_native_texture_record(r,2,1,96,128+r.bytes);
  auto rejected=[&](NativeTextureRecord bad,uint64_t size){bool failed=false;try{validate_native_texture_record(bad,2,1,96,size);}catch(const std::runtime_error&){failed=true;}require(failed,"Malformed texture record accepted");};
  auto bad=r;bad.palette_offset++;rejected(bad,128+r.bytes);
  bad=r;bad.palette_bytes=1024;rejected(bad,128+r.bytes);
  bad=r;bad.width=15;rejected(bad,128+r.bytes);
  bad=r;bad.mips=6;rejected(bad,128+r.bytes);
  bad=r;bad.flags=2;rejected(bad,128+r.bytes);
  rejected(r,128+r.bytes-1);
  bool failed=false;try{decode_native_texture_record(std::array<uint8_t,63>{},2);}catch(const std::runtime_error&){failed=true;}require(failed,"Truncated texture record accepted");
}
}
int main(int argc,char** argv){
  try{
    invalid_records();using namespace aurora::gfx;
    if(argc==2){
      const std::filesystem::path directory=argv[1];NativeTextureArchive pack;pack.open((directory/"native-textures.bin").c_str());
      require(pack.count()==11&&pack.pack_version()==2,"Unexpected texture fixture inventory");
      unsigned levels=0;
      for(uint32_t id=1;id<=pack.count();id++){
        const auto& r=pack.record(id);const auto bytes=pack.payload(id);
        for(uint32_t mip=0;mip<r.mips;mip++){
          const auto expected=read(directory/("asset-"+std::to_string(id)+"-mip-"+std::to_string(mip)+".rgba"));
          require(native_texture_rgba_level(r,bytes,mip)==expected,"Native fixture changed sampled texels");levels++;
        }
      }
      std::printf("Native pack validation passed: 11 formats, %u mips, malformed records rejected\n",levels);
    }else if(argc==3){
      NativeTextureArchive original,prepared;original.open(argv[1]);prepared.open(argv[2]);require(original.count()==prepared.count(),"Texture inventories differ");
      unsigned exact=0,changed=0;
      for(uint32_t id=1;id<=original.count();id++){
        const auto& a=original.record(id);const auto& b=prepared.record(id);require(a.width==b.width&&a.height==b.height&&a.mips==b.mips,"Authored mip dimensions changed");
        const auto old=original.payload(id),current=prepared.payload(id);
        if(b.flags&2){changed++;continue;} // Explicit measured lossy policy; byte proof is separate.
        if(a.format==b.format){require(old==current,"Unchanged native format payload changed");continue;}
        for(uint32_t mip=0;mip<a.mips;mip++)require(native_texture_rgba_level(a,old,mip)==native_texture_rgba_level(b,current,mip),"Exact per-asset policy changed a texel");
        exact++;
      }
      std::printf("Native archive validated: %zu records, %u exact format changes, %u explicit measured choices\n",prepared.count(),exact,changed);
    }else throw std::runtime_error("Usage: native_texture_pack_check FIXTURE_DIR | ORIGINAL_PACK PREPARED_PACK");
    return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"Native texture pack check failed: %s\n",e.what());return 1;}
}
