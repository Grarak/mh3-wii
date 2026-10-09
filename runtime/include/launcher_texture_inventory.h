#pragma once
// Portable preparation metadata only: no guest ABI, asset bytes, hashes or GPU.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace VitaLauncher::TextureInventory {
inline constexpr uint32_t MaxSourceBytes=8*1024*1024,MaxAssets=32768,MaxContainers=4096;
inline constexpr size_t MaxNameBytes=256;
struct Texture {
    uint32_t id,header,offset,bytes,width,height,format,mips,palette,entries,paletteFormat;
    uint32_t containerOffset,containerBytes,archiveEntry,paletteHeader,paletteBytes;
    uint64_t patch;
    std::string name;
};
struct Container {uint32_t offset,bytes,entry;};
namespace detail {
struct Reader {
    std::span<const uint8_t> data;
    size_t lo,hi;
    void Require(size_t at,size_t bytes,size_t begin=0,size_t end=SIZE_MAX) const {
        begin=std::max(begin,lo);end=std::min(end,hi);
        if(bytes>end || at<begin || at>end-bytes)throw std::runtime_error("Resource outside declared container");
    }
    uint16_t U16(size_t at) const {Require(at,2);return uint16_t(data[at])<<8|data[at+1];}
    uint32_t U32(size_t at) const {Require(at,4);return uint32_t(data[at])<<24|uint32_t(data[at+1])<<16|uint32_t(data[at+2])<<8|data[at+3];}
    uint32_t LE32(size_t at) const {Require(at,4);return uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24;}
    size_t Relative(size_t base,size_t field) const {
        const uint32_t raw=U32(field);
        if(!raw)throw std::runtime_error("Missing resource pointer");
        const int64_t target=int64_t(base)+int64_t(raw)-(raw&0x80000000u?int64_t(1)<<32:0);
        if(target<0 || uint64_t(target)>SIZE_MAX)throw std::runtime_error("Invalid signed resource pointer");
        Require(size_t(target),1);return size_t(target);
    }
    std::string Name(size_t at) const {
        Require(at,1);const size_t last=std::min(hi,at+std::min(MaxNameBytes,hi-at));size_t end=at;
        while(end<last && data[end])++end;
        if(end==last)throw std::runtime_error("Unterminated resource name");
        for(size_t i=at;i<end;) {
            const uint8_t first=data[i++];if(first<128)continue;
            const unsigned trailing=first>=0xc2 && first<=0xdf?1:first>=0xe0 && first<=0xef?2:first>=0xf0 && first<=0xf4?3:0;
            if(!trailing || trailing>end-i)throw std::runtime_error("Invalid UTF-8 resource name");
            if((first==0xe0 && data[i]<0xa0) || (first==0xed && data[i]>=0xa0) ||
               (first==0xf0 && data[i]<0x90) || (first==0xf4 && data[i]>=0x90))throw std::runtime_error("Invalid UTF-8 resource name");
            for(unsigned n=0;n<trailing;++n)if((data[i++]&0xc0)!=0x80)throw std::runtime_error("Invalid UTF-8 resource name");
        }
        return {reinterpret_cast<const char*>(data.data()+at),end-at};
    }
};
struct Entry {size_t offset;std::string name;};
inline std::vector<Entry> Dictionary(const Reader& r,size_t at,size_t lo,size_t hi) {
    r.Require(at,24,lo,hi);const uint32_t size=r.U32(at),count=r.U32(at+4);
    if(count>MaxAssets || uint64_t(count+1)*16+8>size)throw std::runtime_error("Invalid resource dictionary size/count");
    r.Require(at,size,lo,hi);std::vector<Entry> entries;entries.reserve(count);std::set<std::string> names;
    for(uint32_t i=0;i<count;++i) {
        const size_t entry=at+24+size_t(i)*16;
        auto name=r.Name(r.Relative(at,entry+8));const size_t target=r.Relative(at,entry+12);
        r.Require(target,1,lo,hi);
        if(name.empty() || !names.insert(name).second)throw std::runtime_error("Empty/duplicate resource dictionary name");
        entries.push_back({target,std::move(name)});
    }
    return entries;
}
struct Resource {size_t offset;uint32_t bytes,magic,version;std::string name;};
inline std::vector<Resource> Resources(const Reader& r) {
    r.Require(r.lo,16);
    if(r.U32(r.lo)!=0x62726573 || r.U16(r.lo+4)!=0xfeff || r.U32(r.lo+8)!=r.hi-r.lo)
        throw std::runtime_error("BRRES header differs from declared container");
    const size_t root=r.lo+r.U16(r.lo+12);r.Require(root,8);
    if(r.U32(root)!=0x726f6f74)throw std::runtime_error("Missing BRRES root");
    const uint32_t rootBytes=r.U32(root+4);r.Require(root,rootBytes);
    std::vector<Resource> result;
    for(const auto& group:Dictionary(r,root+8,root+8,root+rootBytes)) {
        for(auto entry:Dictionary(r,group.offset,r.lo,r.hi)) {
            r.Require(entry.offset,12);const uint32_t size=r.U32(entry.offset+4);
            if(size<12)throw std::runtime_error("Invalid BRRES resource extent");r.Require(entry.offset,size);
            if(result.size()>=MaxAssets)throw std::runtime_error("Too many BRRES resources");
            result.push_back({entry.offset,size,r.U32(entry.offset),r.U32(entry.offset+8),std::move(entry.name)});
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});return result;
}
inline uint64_t TextureBytes(uint32_t format,uint32_t width,uint32_t height,uint32_t mips) {
    uint32_t blockWidth=4,blockHeight=4,blockBytes=32;
    switch(format) {
        case 0:case 8:case 14:blockWidth=8;blockHeight=8;break;
        case 1:case 2:case 9:blockWidth=8;break;
        case 3:case 4:case 5:case 10:break;
        case 6:blockBytes=64;break;
        default:throw std::runtime_error("Unsupported texture source format");
    }
    uint64_t size=0;
    for(uint32_t mip=0;mip<mips;++mip) {
        size+=uint64_t((width+blockWidth-1)/blockWidth)*((height+blockHeight-1)/blockHeight)*blockBytes;
        width=std::max(width/2,1u);height=std::max(height/2,1u);
    }
    return size;
}
}
inline std::vector<Container> Containers(std::span<const uint8_t> source) {
    if(source.size()>MaxSourceBytes)throw std::runtime_error("Oversized texture source");
    const detail::Reader r{source,0,source.size()};r.Require(0,4);
    if(r.U32(0)==0x62726573)return {{0,uint32_t(source.size()),UINT32_MAX}};
    const uint32_t count=r.LE32(0);
    if(!count || count>MaxContainers || count>(source.size()-4)/8)throw std::runtime_error("Invalid resource archive directory");
    const size_t tableEnd=4+size_t(count)*8;std::vector<Container> result;result.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        const uint32_t at=r.LE32(4+size_t(i)*8),size=r.LE32(8+size_t(i)*8);
        if(at<tableEnd || at%64 || size<128 || size%128)throw std::runtime_error("Invalid archive entry offset/length");
        r.Require(at,size);
        if(r.U32(at)!=0x62726573 || r.U16(at+4)!=0xfeff || r.U32(at+8)!=size)
            throw std::runtime_error("Archive member header differs from directory");
        result.push_back({at,size,i});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    for(size_t i=1;i<result.size();++i)if(uint64_t(result[i-1].offset)+result[i-1].bytes>result[i].offset)
        throw std::runtime_error("Overlapping resource archive entries");
    return result;
}
inline std::vector<Texture> Parse(std::span<const uint8_t> source,uint64_t partitionOffset,uint32_t firstId=1) {
    if(partitionOffset>UINT64_MAX-source.size() || !firstId || firstId>MaxAssets)
        throw std::runtime_error("Invalid texture source partition extent/ID");
    std::vector<Texture> result;
    for(const auto& container:Containers(source)) {
        const detail::Reader r{source,container.offset,size_t(container.offset)+container.bytes};
        const auto resources=detail::Resources(r);
        struct Palette {uint32_t offset,entries,format,header,bytes;};std::map<std::string,Palette> palettes;
        for(const auto& resource:resources) {
            if(resource.magic!=0x54455830 && resource.magic!=0x504c5430)continue;
            if(resource.magic==0x54455830 && resource.version==1)continue;
            const size_t at=resource.offset;r.Require(at,64,at,at+resource.bytes);
            if(resource.version!=3 || r.Name(r.Relative(at,at+20))!=resource.name)
                throw std::runtime_error("Unsupported/mismatched texture resource");
            if(resource.magic==0x504c5430) {
                const size_t start=r.Relative(at,at+16);const uint32_t entries=r.U16(at+28),format=r.U32(at+24);
                if(format>2)throw std::runtime_error("Invalid palette format");r.Require(start,size_t(entries)*2,at+64,at+resource.bytes);
                if(!palettes.emplace(resource.name,Palette{uint32_t(start),entries,format,uint32_t(at),resource.bytes}).second)
                    throw std::runtime_error("Duplicate palette resource name");
            }
        }
        for(const auto& resource:resources) {
            if(resource.magic!=0x54455830 || resource.version==1)continue;
            const size_t at=resource.offset,start=r.Relative(at,at+16);
            const uint32_t width=r.U16(at+28),height=r.U16(at+30),format=r.U32(at+32),mips=r.U32(at+36);
            if(start!=at+64 || r.U32(at+60) || !width || !height || width>2048 || height>2048 ||
               !mips || mips>unsigned(std::bit_width(std::max(width,height))))throw std::runtime_error("Unsupported texture dimensions/header");
            if(detail::TextureBytes(format,width,height,mips)>at+resource.bytes-start)
                throw std::runtime_error("Truncated texture mip payload");
            Palette palette{};const auto p=palettes.find(resource.name);if(p!=palettes.end())palette=p->second;
            if((format==8 || format==9 || format==10) && !palette.entries)throw std::runtime_error("Missing texture palette");
            if(result.size()>MaxAssets-firstId)throw std::runtime_error("Too many texture assets");
            result.push_back({firstId+uint32_t(result.size()),uint32_t(at),uint32_t(start),uint32_t(at+resource.bytes-start),
                width,height,format,mips,palette.offset,palette.entries,palette.format,container.offset,container.bytes,container.entry,palette.header,palette.bytes,
                partitionOffset+at+60,resource.name});
        }
    }
    return result;
}
inline std::vector<std::string> SourceNames(const std::vector<std::string>& files) {
    std::vector<std::string> standalone,archives;
    for(const auto& file:files) {
        if(file.ends_with(".brres"))standalone.push_back(file);
        else {
            const auto slash=file.find_last_of('/');const auto leaf=file.substr(slash==file.npos?0:slash+1);
            if(file=="11/efm/com/efm_com.bin" || file=="11/efm/com/efm_lc_com.bin" ||
               (file.starts_with("15/") && leaf.size()==13 && leaf.starts_with("efm_map") &&
                leaf[7]>='0' && leaf[7]<='9' && leaf[8]>='0' && leaf[8]<='9' && leaf.ends_with(".bin")))archives.push_back(file);
        }
    }
    std::sort(standalone.begin(),standalone.end());std::sort(archives.begin(),archives.end());
    standalone.insert(standalone.end(),archives.begin(),archives.end());return standalone;
}
inline void ValidateSource(std::span<const uint8_t> source,uint64_t partitionOffset,const Texture& texture) {
    if(source.size()>MaxSourceBytes || partitionOffset>UINT64_MAX-source.size())throw std::runtime_error("Invalid texture source reread");
    if(uint64_t(texture.containerOffset)+texture.containerBytes>source.size())throw std::runtime_error("Truncated texture source reread");
    const detail::Reader r{source,texture.containerOffset,size_t(texture.containerOffset)+texture.containerBytes};
    if(texture.archiveEntry!=UINT32_MAX) {
        const detail::Reader outer{source,0,source.size()};const size_t entry=4+size_t(texture.archiveEntry)*8;
        if(texture.archiveEntry>=outer.LE32(0) || outer.LE32(entry)!=texture.containerOffset || outer.LE32(entry+4)!=texture.containerBytes)
            throw std::runtime_error("Texture archive directory changed before conversion");
    }
    r.Require(texture.header,64);r.Require(texture.offset,texture.bytes);
    if(r.U32(r.lo)!=0x62726573 || r.U16(r.lo+4)!=0xfeff || r.U32(r.lo+8)!=texture.containerBytes || r.U32(texture.header)!=0x54455830 ||
       r.U32(texture.header+4)!=texture.bytes+64 || r.U32(texture.header+8)!=3 || r.Relative(texture.header,texture.header+16)!=texture.offset ||
       r.U16(texture.header+28)!=texture.width || r.U16(texture.header+30)!=texture.height ||
       r.U32(texture.header+32)!=texture.format || r.U32(texture.header+36)!=texture.mips ||
       r.U32(texture.header+60) || partitionOffset+texture.header+60!=texture.patch)
        throw std::runtime_error("Texture metadata/provenance changed before conversion");
    if(texture.entries) {
        r.Require(texture.paletteHeader,64);r.Require(texture.paletteHeader,texture.paletteBytes);
        r.Require(texture.palette,size_t(texture.entries)*2,texture.paletteHeader+64,size_t(texture.paletteHeader)+texture.paletteBytes);
        if(r.U32(texture.paletteHeader)!=0x504c5430 || r.U32(texture.paletteHeader+4)!=texture.paletteBytes || r.U32(texture.paletteHeader+8)!=3 ||
           r.Relative(texture.paletteHeader,texture.paletteHeader+16)!=texture.palette ||
           r.U32(texture.paletteHeader+24)!=texture.paletteFormat || r.U16(texture.paletteHeader+28)!=texture.entries)
            throw std::runtime_error("Texture palette metadata changed before conversion");
    }
}
}
