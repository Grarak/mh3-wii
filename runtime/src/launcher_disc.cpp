#include "vita_launcher.h"
#include "disc_image.h"
#include "launcher_texture_inventory.h"
#include "gfx/native_texture_pack.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#ifdef MH3_HAS_ROM_DECODER
#include "../vita/rom_decoder.h"
#endif
#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VitaLauncher {
namespace {
class File {
public:
    uint64_t size=0;
    explicit File(const std::filesystem::path& path) {
#ifdef __vita__
        fd=sceIoOpen(path.string().c_str(),SCE_O_RDONLY,0); SceIoStat st{};
        const int result=fd<0?-1:sceIoGetstatByFd(fd,&st);
#else
        fd=::open(path.c_str(),O_RDONLY); struct stat st{};
        const int result=fd<0?-1:fstat(fd,&st);
#endif
        if(result<0) {Close();throw std::runtime_error("Cannot read ROM: "+path.string());}
        size=st.st_size;
    }
    ~File(){Close();}
    void Read(uint64_t at,uint8_t* bytes,size_t count) const {
        if(at>size || count>size-at || at>INT64_MAX) throw std::runtime_error("ROM is truncated");
        size_t done=0;
        while(done<count) {
#ifdef __vita__
            const auto n=sceIoPread(fd,bytes+done,count-done,at+done);
#else
            const auto n=pread(fd,bytes+done,count-done,at+done);
#endif
            if(n<=0) throw std::runtime_error("ROM read failed");
            done+=n;
        }
    }
    std::vector<uint8_t> Read(uint64_t at,size_t count) const {std::vector<uint8_t> b(count);Read(at,b.data(),count);return b;}
private:
    int fd=-1;
    void Close(){if(fd<0)return;
#ifdef __vita__
        sceIoClose(fd);
#else
        ::close(fd);
#endif
        fd=-1;
    }
};
// nod handles RVZ/WIA decompression and Wii title-key/AES decoding. Native
// callbacks retain full 64-bit source offsets on Vita's 32-bit newlib.
class Rom {
public:
    uint64_t size=0;
    explicit Rom(const std::filesystem::path& path):file(path) {
        size=file.size;
#ifdef MH3_HAS_ROM_DECODER
        const auto header=file.Read(0,0x100);
        // nod 1.4 treats plain ISO sectors as encrypted. nodtool's decrypted
        // ISO marks byte 0x61; retain the bounded native sector reader for it.
        if(header[0x61]==1 && std::string(header.begin(),header.begin()+6)=="RMHP08")return;
        std::array<uint8_t,512> error{};
        decoder=mh3_rom_open(&file,file.size,[](void* context,uint64_t at,uint8_t* bytes,size_t count)->intptr_t {
            try {static_cast<File*>(context)->Read(at,bytes,count);return count;}catch(...){return -1;}
        },error.data(),error.size());
        if(!decoder)throw std::runtime_error(reinterpret_cast<const char*>(error.data()));
        size=mh3_rom_size(decoder,0);
#endif
    }
    ~Rom(){CloseDecoder();}
    void CloseDecoder(){
#ifdef MH3_HAS_ROM_DECODER
        mh3_rom_close(decoder);decoder=nullptr;
#endif
    }
    void Partition(uint64_t at,uint64_t bytes){start=at;flatSize=bytes/0x8000*0x7c00;
#ifdef MH3_HAS_ROM_DECODER
        if(decoder && flatSize!=mh3_rom_size(decoder,1))throw std::runtime_error("Decoded partition extent differs from its header");
#endif
    }
    uint64_t FlatSize()const{return flatSize;}
    void Read(uint64_t at,uint8_t* bytes,size_t count,bool flat=false)const {
#ifdef MH3_HAS_ROM_DECODER
        if(decoder) {
            std::array<uint8_t,512> error{};
            if(mh3_rom_read(decoder,flat,at,bytes,count,error.data(),error.size())<0)
                throw std::runtime_error(reinterpret_cast<const char*>(error.data()));
            return;
        }
#endif
        if(!flat){file.Read(at,bytes,count);return;}
        size_t done=0;
        while(done<count){const size_t amount=std::min<size_t>(count-done,0x7c00-at%0x7c00);file.Read(start+at/0x7c00*0x8000+0x400+at%0x7c00,bytes+done,amount);at+=amount;done+=amount;}
    }
    std::vector<uint8_t> Read(uint64_t at,size_t count,bool flat=false)const {std::vector<uint8_t> b(count);Read(at,b.data(),count,flat);return b;}
private:
    File file;uint64_t start=0,flatSize=0;
#ifdef MH3_HAS_ROM_DECODER
    void* decoder=nullptr;
#endif
};
uint32_t Word(const std::vector<uint8_t>& b,size_t at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("ROM metadata is truncated");
    return uint32_t(b[at])<<24|uint32_t(b[at+1])<<16|uint32_t(b[at+2])<<8|b[at+3];
}
void Write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();
    if(!out)throw std::runtime_error("Cannot write prepared ROM metadata");
}
template<class Input> void Copy(const Input& file,uint64_t at,uint64_t count,const std::filesystem::path& path) {
    if(count>16*1024*1024)throw std::runtime_error("ROM metadata exceeds the memory limit");
    Write(path,file.Read(at,count));
}
}
bool DiscReady(const std::filesystem::path& output) {
    try {
        File boot(output/"sys/boot.bin");const auto id=boot.Read(0,6);
        if(std::string(id.begin(),id.end())!="RMHP08")return false;
        DiscImage disc(output,false);
        File pack(output/"native-textures.bin"), map(output/"native-texture-map.bin");
        if(pack.size<32 || map.size<12)return false;
        const auto header=pack.Read(0,32), tags=map.Read(0,12);
        if(std::string(header.begin(),header.begin()+4)!="M3VT" || std::string(tags.begin(),tags.begin()+4)!="M3TM")return false;
        // Pack headers are little endian. Validate record extents as well as
        // exact table sizes; a half-written payload must never be considered ready.
        const auto le=[](const std::vector<uint8_t>& b,size_t at){return uint32_t(b.at(at))|uint32_t(b.at(at+1))<<8|uint32_t(b.at(at+2))<<16|uint32_t(b.at(at+3))<<24;};
        const uint32_t count=le(header,8),version=le(header,4),stride=le(header,12);
        if((version!=1 && version!=2) || stride!=(version==1?40u:64u) || count>TextureInventory::MaxAssets ||
           le(header,16) || le(header,20) || le(header,24) || le(header,28) || le(tags,4)!=1 || le(tags,8)!=count ||
           map.size!=12+uint64_t(count)*12 || pack.size<32+uint64_t(count)*stride)return false;
        const auto records=pack.Read(32,count*stride);
        uint64_t end=32+uint64_t(count)*stride;
        for(uint32_t i=0;i<count;++i) {
            const auto record=aurora::gfx::decode_native_texture_record({records.data()+size_t(i)*stride,stride},version);
            aurora::gfx::validate_native_texture_record(record,version,i+1,end,pack.size);
            end=record.offset+record.bytes;
        }
        const auto files=disc.FileNames();std::vector<std::pair<uint64_t,uint32_t>> ranges;
        for(const auto& path:files)ranges.emplace_back(disc.FileOffset(path),disc.FileSize(path));
        std::sort(ranges.begin(),ranges.end());
        const auto patches=map.Read(12,count*12);std::vector<bool> ids(count);
        uint64_t previous=0;size_t range=0;
        for(uint32_t i=0;i<count;++i) {
            const uint64_t offset=le(patches,i*12)|uint64_t(le(patches,i*12+4))<<32;
            const uint32_t tag=Word(patches,i*12+8),id=tag&0x0fffffff;
            if((tag&0xf0000000)!=0xa0000000 || !id || id>count || ids[id-1] ||
               offset>UINT64_MAX-4 || (i && offset<previous+4))return false;
            while(range<ranges.size() && offset>=ranges[range].first+ranges[range].second)++range;
            if(range==ranges.size() || offset<ranges[range].first ||
               ranges[range].second<4 || offset-ranges[range].first>ranges[range].second-4)return false;
            ids[id-1]=true;previous=offset;
        }
        // Old standalone-only preparations have valid tables but omit effect
        // resources. Check the exact appended dictionary inventory and IDs.
        std::vector<TextureInventory::Texture> required;
        for(const auto& path:TextureInventory::SourceNames(files)) {
            if(path.ends_with(".brres"))continue;
            if(disc.FileSize(path)>TextureInventory::MaxSourceBytes)return false;
            const auto bytes=disc.ReadFile(path);
            for(const auto& texture:TextureInventory::Parse(bytes,disc.FileOffset(path))) {
                if(required.size()>=TextureInventory::MaxAssets)return false;
                required.push_back(texture);
            }
        }
        if(required.size()>count)return false;
        const uint32_t first=count-uint32_t(required.size())+1;
        for(size_t i=0;i<required.size();++i) {
            const auto& texture=required[i];
            uint32_t lo=0,hi=count;
            while(lo<hi) {
                const uint32_t mid=lo+(hi-lo)/2;
                const uint64_t offset=le(patches,mid*12)|uint64_t(le(patches,mid*12+4))<<32;
                if(offset<texture.patch)lo=mid+1;else hi=mid;
            }
            if(lo==count || (le(patches,lo*12)|uint64_t(le(patches,lo*12+4))<<32)!=texture.patch ||
               Word(patches,lo*12+8)!=(0xa0000000|uint32_t(first+i)))return false;
            const auto record=aurora::gfx::decode_native_texture_record({records.data()+size_t(first+i-1)*stride,stride},version);
            if(record.width!=texture.width || record.height!=texture.height || record.mips!=texture.mips)return false;
            if(version==2 && (record.source_format!=texture.format || record.source_palette_entries!=texture.entries ||
               record.source_palette_format!=texture.paletteFormat ||
               record.source_palette_delta!=(texture.entries?int64_t(texture.palette)-texture.offset:0)))return false;
        }
        return true;
    } catch(const std::exception&) {return false;}
}
void ProcessDisc(const std::filesystem::path& iso,const std::filesystem::path& output,const Progress& progress) {
    if(DiscReady(output))return;
    if(std::filesystem::exists(output)) {
        if(!std::filesystem::exists(output/"disc-layout.txt"))throw std::runtime_error("Processing output contains unrelated files: "+output.string());
        auto previous=std::filesystem::path(output.string()+".incomplete");unsigned serial=1;
        while(std::filesystem::exists(previous))previous=output.string()+".incomplete-"+std::to_string(serial++);
        std::filesystem::rename(output,previous);
    }
    Rom source(iso);
    const auto discHeader=source.Read(0,0x100);
    if(std::string(discHeader.begin(),discHeader.begin()+6)!="RMHP08" || Word(discHeader,0x18)!=0x5d1c9ea3)
        throw std::runtime_error("Expected the European RMHP08 Wii disc image.");
    const auto groups=source.Read(0x40000,32);
    uint64_t partition=0;unsigned found=0;
    for(unsigned group=0;group<4;++group) {
        const uint32_t count=Word(groups,group*8);const uint64_t table=uint64_t(Word(groups,group*8+4))*4;
        if(count>256)throw std::runtime_error("Invalid Wii partition table");
        const auto entries=source.Read(table,count*8);
        for(unsigned i=0;i<count;++i)if(Word(entries,i*8+4)==0){partition=uint64_t(Word(entries,i*8))*4;++found;}
    }
    if(found!=1)throw std::runtime_error("Expected exactly one game partition");
    const auto partitionHeader=source.Read(partition,0x2c0);
    const uint64_t start=partition+uint64_t(Word(partitionHeader,0x2b8))*4,size=uint64_t(Word(partitionHeader,0x2bc))*4;
    if(!size || size%0x8000 || start>source.size || size>source.size-start)throw std::runtime_error("Invalid Wii partition extent");
    source.Partition(start,size);
    const auto boot=source.Read(0,0x440,true);
    if(std::string(boot.begin(),boot.begin()+6)!="RMHP08" || Word(boot,0x18)!=0x5d1c9ea3)
        throw std::runtime_error("ISO partition is encrypted. Convert with nodtool convert --decrypt before transferring.");
    const auto staging=std::filesystem::path(output.string()+".processing");
    // Only this launcher's named staging directory is disposable; no input,
    // saves or existing published game directory is ever removed.
    if(std::filesystem::exists(staging))std::filesystem::remove_all(staging);
    std::filesystem::create_directories(staging/"sys");
    try {
        const uint64_t flatSize=source.FlatSize();
        progress("Decoding Wii partition",0,flatSize);
        std::ofstream out(staging/"partition.bin",std::ios::binary|std::ios::trunc);
        std::array<uint8_t,0x7c00> sector{};
        for(uint64_t done=0;done<flatSize;done+=sector.size()) {
            source.Read(done,sector.data(),sector.size(),true);
            out.write(reinterpret_cast<const char*>(sector.data()),sector.size());
            if(!out)throw std::runtime_error("Not enough space or a partition write failed");
            if(done%(0x7c00*128)==0)progress("Decoding Wii partition",done,flatSize);
        }
        out.close();if(!out)throw std::runtime_error("Closing prepared partition failed");
        File flat(staging/"partition.bin");
        Write(staging/"sys/boot.bin",boot);Copy(flat,0x440,0x2000,staging/"sys/bi2.bin");
        const auto app=flat.Read(0x2440,0x20);
        Copy(flat,0x2440,0x20+uint64_t(Word(app,0x14))+Word(app,0x18),staging/"sys/apploader.img");
        const uint64_t dolAt=uint64_t(Word(boot,0x420))*4;
        const auto dol=flat.Read(dolAt,0x100);uint64_t dolSize=0x100;
        for(unsigned i=0;i<18;++i)dolSize=std::max(dolSize,uint64_t(Word(dol,i*4))+Word(dol,0x90+i*4));
        Copy(flat,dolAt,dolSize,staging/"sys/main.dol");
        const uint64_t fstAt=uint64_t(Word(boot,0x424))*4,fstSize=uint64_t(Word(boot,0x428))*4;
        if(fstSize<12 || fstSize>1024*1024)throw std::runtime_error("Invalid FST size");
        Copy(flat,fstAt,fstSize,staging/"sys/fst.bin");
        Copy(source,partition,0x2a4,staging/"ticket.bin");
        Copy(source,partition+uint64_t(Word(partitionHeader,0x2a8))*4,Word(partitionHeader,0x2a4),staging/"tmd.bin");
        Copy(source,partition+uint64_t(Word(partitionHeader,0x2b0))*4,Word(partitionHeader,0x2ac),staging/"cert.bin");
        Copy(source,partition+uint64_t(Word(partitionHeader,0x2b4))*4,0x18000,staging/"h3.bin");
        source.CloseDecoder(); // Release decompression buffers before texture conversion.
        Write(staging/"disc-layout.txt",{'m','h','3','-','p','a','r','t','i','t','i','o','n','-','v','1','\n'});
        DiscImage validation(staging);
        PrepareTextures(staging,progress);
        if(!DiscReady(staging))throw std::runtime_error("Prepared disc validation failed");
#ifdef __vita__
        if(sceIoSync("ux0:",0)<0)throw std::runtime_error("Cannot sync prepared game data");
#endif
        std::filesystem::rename(staging,output);
        progress("Game data ready",1,1);
    } catch(...) {
        // An interrupted or failed stage is never published as ready. Keep it
        // for diagnostics; the next processing attempt recreates this stage.
        throw;
    }
}
}
