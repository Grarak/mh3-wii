// Typed layout and character writer ABI adapters for NW4R and NW4HBM.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "native_graphics.h"
#include "../native/mh3_system_ui.h"
#include "../native/mh3_material_state.h"
#include "../native/adapters/model_state_bridge.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace {
struct VertexFormat {bool colors=true;uint8_t coordinates=1;};
VertexFormat format;
std::unordered_map<uint32_t,uint32_t> palette_headers;
std::unordered_map<uint32_t,mh3_native_texture_source> font_sheets;
uint64_t producer_generation=0;
mh3_system_ui_layout layout{};
float layout_matrices[10][12]{};
bool layout_coordinates=false;
void Checked(mh3_res_status status) {if(status!=MH3_RES_OK)throw std::runtime_error("Native system UI input: "+std::to_string(status));}
mh3_res_span Bytes(uint32_t address,size_t count){return {Memory::GetPointer(address,count),count};}
mh3_res_mut_span Mutable(uint32_t address,size_t count){return {Memory::GetPointer(address,count),count};}
void Floats(uint32_t address,float*out,unsigned count){for(unsigned i=0;i<count;++i)out[i]=Memory::ReadFloat32(address+i*4);}
void Colors(uint32_t address,uint8_t out[4][4]){for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)out[i][j]=address?Memory::Read8(address+i*4+j):255;}
uint64_t Produced(uint32_t payload) {
    producer_generation=std::max(producer_generation,ReadNativeTextureGeneration(payload))+1;
    if(!producer_generation)throw std::runtime_error("System UI producer revision overflow");
    MarkNativeTextureProduced(payload,producer_generation);return producer_generation;
}
void Submit(const mh3_system_ui_vertex*source,uint32_t count) {
    if(!NativeGraphicsFrameShouldRender())return;
    AuroraNativeMaterialGeometry geometry{};
    if(!aurora_allocate_native_material_geometry(count,count/4*6,&geometry))return;
    if(layout_coordinates){
        std::vector<mh3_system_ui_vertex> transformed(source,source+count);
        Checked(mh3_system_ui_coordinates(transformed.data(),count,&layout,layout_matrices));
        Checked(mh3_system_ui_build(&GetNativeDrawState(),transformed.data(),count,&geometry));
    }else Checked(mh3_system_ui_build(&GetNativeDrawState(),source,count,&geometry));
    SubmitNativeMaterialGeometry(geometry);
}
void Pipeline(mh3_system_ui_pipeline mode,const uint8_t*first=nullptr,const uint8_t*second=nullptr,bool hbm=false) {
    auto material=GetNativeMaterialState();mh3_system_ui_material(&material,mode,first,second);
    material.fog_reference=-1;SetNativeMaterial(material);mh3_native_fog fog{};auto*rgba=reinterpret_cast<uint8_t*>(&fog.color);for(unsigned i=0;i<4;++i)rgba[i]=Memory::Read8((hbm?0x8086824c:0x80797c2c)+i);SetNativeFog(fog);
    format={true,1};
    layout_coordinates=false;
}
void FontGlobal(GuestCall&call,bool hbm) {
    const uint32_t flag=hbm?0x80868248:0x80797c28,color=hbm?0x8086824c:0x80797c2c;
    if(!Memory::Read8(flag)) {
        Memory::Write32(color,0);
        if(!hbm)call(0x80456D24,{color,0x8005B28C,0x80763158});
        Memory::Write8(flag,1);
    }
}
void FontSetup(CpuContext*cpu,bool hbm) {
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};const auto writer=frame.Arg(3);
    Memory::Write32(hbm?0x80868250:0x80763168,255);Memory::Write32(hbm?0x80868254:0x8076316c,0);
    FontGlobal(call,hbm);
    if(Memory::Read32(writer)!=0 || Memory::Read32(writer+4)!=0xffffffffu) {
        uint8_t colors[2][4];for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)colors[i][j]=Memory::Read8(writer+i*4+j);
        Pipeline(MH3_SYSTEM_UI_COLOR_MAPPING,colors[0],colors[1],hbm);
    } else {
        const uint32_t font=Memory::Read32(writer+0x48);
        const uint32_t texture_format=font?call.Virtual(font,0x30):14;
        Pipeline(texture_format<2?MH3_SYSTEM_UI_INTENSITY:MH3_SYSTEM_UI_MODULATE,nullptr,nullptr,hbm);
    }
}
mh3_native_texture_binding GlyphBinding(uint32_t writer,uint32_t glyph,uint32_t unit) {
    mh3_native_texture_binding binding{};binding.map_id=unit;
    const auto found=font_sheets.find(Memory::Read32(glyph));
    if(found==font_sheets.end())throw std::runtime_error("System UI font atlas has no explicit font resource producer");
    binding.source=found->second;
    if(binding.source.width!=Memory::Read16(glyph+12) || binding.source.height!=Memory::Read16(glyph+14) || binding.source.format!=Memory::Read32(glyph+8))
        throw std::runtime_error("System UI glyph disagrees with its font sheet producer");
    binding.sampler.min_filter=Memory::Read32(writer+0x38);binding.sampler.mag_filter=Memory::Read32(writer+0x3c);
    return binding;
}
void GlyphTexture(uint32_t writer,uint32_t glyph,uint32_t unit,bool hbm) {
    const uint32_t cache=hbm?0x80868250:0x80763168;
    auto binding=GlyphBinding(writer,glyph,unit);
    const auto bound=GetNativeTextureBinding(unit);
    if(!bound || bound->source.generation!=binding.source.generation || unit!=Memory::Read32(cache) || binding.source.payload_address!=Memory::Read32(cache+4) ||
       binding.sampler.min_filter!=Memory::Read32(cache+8) || binding.sampler.mag_filter!=Memory::Read32(cache+12)) {
        BindNativeTexture(binding);Memory::Write32(cache,unit);Memory::Write32(cache+4,binding.source.payload_address);
        Memory::Write32(cache+8,binding.sampler.min_filter);Memory::Write32(cache+12,binding.sampler.mag_filter);
    }
}
void Glyph(CpuContext*cpu,bool hbm) {
    const auto writer=cpu->gpr[3],glyph=cpu->gpr[4];const float origin[3]={float(cpu->fpr[1].d),float(cpu->fpr[2].d),float(cpu->fpr[3].d)};
    GlyphTexture(writer,glyph,0,hbm);
    if(!NativeGraphicsFrameShouldRender())return;
    uint8_t colors[4][4];Colors(writer+8,colors);mh3_system_ui_vertex vertices[4];
    if(!mh3_system_ui_glyph(vertices,origin,Memory::Read8(glyph+5),Memory::Read8(glyph+7),
        Memory::ReadFloat32(writer+0x24),Memory::ReadFloat32(writer+0x28),Memory::Read16(glyph+12),Memory::Read16(glyph+14),
        Memory::Read16(glyph+16),Memory::Read16(glyph+18),colors))throw std::runtime_error("System UI glyph atlas has zero dimensions");
    Submit(vertices,4);
}
void LoadPane(uint32_t pane,uint32_t draw_info,int y_mode) {
    float matrix[12];Floats(pane+0x84,matrix,12);
    if(int8_t(Memory::Read8(draw_info+0x50))<0) {
        float left[12];Floats(draw_info+4,left,12);mh3_system_ui_concat(matrix,left,matrix);
    }
    if(y_mode==1 || (y_mode==2 && Memory::ReadFloat32(draw_info+0x40)<Memory::ReadFloat32(draw_info+0x38)))
        for(unsigned i=0;i<3;++i)matrix[i*4+1]=-matrix[i*4+1];
    SetNativePositionMatrix(matrix);
}
void Quad(CpuContext*cpu,int y_sign) {
    const uint32_t origin_address=cpu->gpr[3],size_address=cpu->gpr[4],count=uint8_t(cpu->gpr[5]),coords=cpu->gpr[6],colors_address=cpu->gpr[7];
    if(count>8 || count!=format.coordinates || bool(colors_address)!=format.colors)
        throw std::runtime_error("Layout quad disagrees with authored vertex format");
    if(!NativeGraphicsFrameShouldRender())return;
    float origin[3]={},size[2],uv[8][4][2]={};uint8_t colors[4][4];
    Floats(origin_address,origin,2);Floats(size_address,size,2);Colors(colors_address,colors);
    for(unsigned i=0;i<count;++i)for(unsigned j=0;j<4;++j)Floats(coords+i*32+j*8,uv[i][j],2);
    mh3_system_ui_vertex vertices[4];mh3_system_ui_quad(vertices,origin,size,y_sign,uv,count,colors);Submit(vertices,4);
}
void ConstructTPL(CpuContext*cpu,uint32_t object,uint32_t tpl,uint32_t index,bool full_lod) {
    GuestFrame frame(cpu,0x20);GuestCall call{cpu};
    if(Memory::Read32(tpl+8)<0x80000000u)call(0x804E4C30,{tpl});
    const uint32_t descriptor=call(0x804E4D50,{tpl,index}),header=Memory::Read32(descriptor),palette=Memory::Read32(descriptor+4);
    mh3_ui_texture_header image{};Checked(mh3_ui_texture_header_read(Bytes(header,36),&image));mh3_native_texture_binding binding{};
    binding.source.resource_address=header;binding.source.payload_address=image.payload_address;binding.source.width=image.width;binding.source.height=image.height;
    binding.source.format=image.format;binding.source.indexed=palette!=0;binding.source.generation=Produced(image.payload_address);
    binding.source.min_lod=full_lod?Memory::Read8(header+0x21):0;binding.source.max_lod=full_lod?Memory::Read8(header+0x22):0;
    binding.source.levels=full_lod&&binding.source.min_lod!=binding.source.max_lod?uint32_t(binding.source.max_lod)+1:1;
    binding.sampler.wrap_s=image.wrap_s;binding.sampler.wrap_t=image.wrap_t;
    binding.sampler.min_filter=full_lod?Memory::Read32(header+0x14):1;binding.sampler.mag_filter=full_lod?Memory::Read32(header+0x18):1;
    binding.sampler.lod_bias=full_lod?Memory::ReadFloat32(header+0x1c):0;binding.sampler.edge_lod=full_lod?Memory::Read8(header+0x20):0;
    Checked(mh3_system_ui_texture_lod(&binding));if(full_lod&&Memory::Read8(header+0x21)!=Memory::Read8(header+0x22))binding.source.levels=uint32_t(binding.source.max_lod)+1;StoreNativeTextureObject(object,binding);palette_headers[object]=palette;Memory::Write32(object+16,palette);
    if(palette)Produced(Memory::Read32(palette+8));
}
void CopyTexture(uint32_t destination,uint32_t source,bool preserve_wrap,bool clear_palette) {
    auto binding=ReadNativeTextureObject(source);
    if(preserve_wrap){const auto previous=ReadNativeTextureObject(destination);binding.sampler.wrap_s=previous.sampler.wrap_s;binding.sampler.wrap_t=previous.sampler.wrap_t;}
    StoreNativeTextureObject(destination,binding);
    palette_headers[destination]=clear_palette?0:palette_headers.at(source);Memory::Write32(destination+16,palette_headers[destination]);
}
uint32_t TextureSlot(uint32_t material,uint32_t slot) {return Memory::Read32(material+0x58)+uint8_t(slot)*32;}
uint32_t Field(void*,uint32_t self,uint32_t offset){return Memory::Read32(self+offset);}
uint32_t Method(void*user,uint32_t object,uint32_t slot){return static_cast<GuestCall*>(user)->Virtual(object,slot);}
void SaveDraw(CpuContext*cpu,unsigned variant) {
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};auto raster=GetNativeDrawState().raster;
    raster.depth_test=1;raster.depth_compare=variant==1?7:3;raster.depth_write=1;SetNativeRaster(raster);
    const mh3_system_ui_callbacks callbacks{&call,Field,Method};mh3_system_ui_save_draw(&callbacks,frame.Arg(3),variant);
}
void PublishFont(uint32_t font,uint32_t resource,uint32_t information) {
    Memory::Write32(font+16,resource);Memory::Write32(font+20,information);
    const uint32_t sheets=Memory::Read32(information+8),payload=Memory::Read32(sheets+20),stride=Memory::Read32(sheets+4),count=Memory::Read16(sheets+8);
    const uint64_t resource_end=uint64_t(resource)+Memory::Read32(resource+8);
    if(!stride || !count || payload<resource || uint64_t(payload)+uint64_t(stride)*count>resource_end)
        throw std::runtime_error("System UI font sheets exceed their resource allocation");
    for(uint32_t i=0;i<count;++i) {
        mh3_native_texture_source source{};source.resource_address=resource;source.payload_address=payload+i*stride;
        source.width=Memory::Read16(sheets+16);source.height=Memory::Read16(sheets+18);source.format=Memory::Read16(sheets+10);source.levels=1;
        source.generation=Produced(source.payload_address);font_sheets.insert_or_assign(source.payload_address,source);
    }
}
}
extern "C" void MH3_SystemUIFontResource(CpuContext*cpu){PublishFont(cpu->gpr[3],cpu->gpr[4],cpu->gpr[5]);}
REGISTER_NATIVE_FUNCTION_AS(0x80503024,MH3_SystemUIFontResource,"SystemUIFontResource");
REGISTER_NATIVE_FUNCTION_AS(0x8083A1D0,MH3_SystemUIFontResource,"HBMFontResource");
extern "C" void MH3_SystemUIFontSetup(CpuContext*cpu){FontSetup(cpu,false);}
REGISTER_NATIVE_FUNCTION_AS(0x80503C38,MH3_SystemUIFontSetup,"SystemUIFontSetup");
extern "C" void MH3_HBMFontSetup(CpuContext*cpu){FontSetup(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x80837290,MH3_HBMFontSetup,"HBMFontSetup");
namespace {
void FontMapping(CpuContext*cpu,bool hbm){GuestFrame frame(cpu,0x10);GuestCall call{cpu};FontGlobal(call,hbm);uint8_t c[2][4];for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)c[i][j]=Memory::Read8(frame.Arg(3+i)+j);Pipeline(MH3_SYSTEM_UI_COLOR_MAPPING,c[0],c[1],hbm);}
void FontDefault(CpuContext*cpu,mh3_system_ui_pipeline mode){GuestFrame frame(cpu,0x10);GuestCall call{cpu};FontGlobal(call,true);Pipeline(mode,nullptr,nullptr,true);}
}
extern "C" void MH3_SystemUIFontMapping(CpuContext*cpu){FontMapping(cpu,false);}
REGISTER_NATIVE_FUNCTION_AS(0x80504D70,MH3_SystemUIFontMapping,"SystemUIFontMapping");
extern "C" void MH3_HBMFontMapping(CpuContext*cpu){FontMapping(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x808390D0,MH3_HBMFontMapping,"HBMFontMapping");
extern "C" void MH3_SystemUIFontDefault(CpuContext*cpu){FontDefault(cpu,MH3_SYSTEM_UI_MODULATE);}
REGISTER_NATIVE_FUNCTION_AS(0x80838F00,MH3_SystemUIFontDefault,"HBMFontDefault");
REGISTER_NATIVE_FUNCTION_AS(0x80839600,MH3_SystemUIFontDefault,"HBMFontRGBA");
extern "C" void MH3_SystemUIFontIntensity(CpuContext*cpu){FontDefault(cpu,MH3_SYSTEM_UI_INTENSITY);}
REGISTER_NATIVE_FUNCTION_AS(0x808393D0,MH3_SystemUIFontIntensity,"HBMFontIntensity");
extern "C" void MH3_SystemUIFontGlyph(CpuContext*cpu){Glyph(cpu,false);}
REGISTER_NATIVE_FUNCTION_AS(0x805049A4,MH3_SystemUIFontGlyph,"SystemUIFontGlyph");
extern "C" void MH3_HBMFontGlyph(CpuContext*cpu){Glyph(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x80838970,MH3_HBMFontGlyph,"HBMFontGlyph");
extern "C" void MH3_HBMFontTexture(CpuContext*cpu){GlyphTexture(cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],true);}
REGISTER_NATIVE_FUNCTION_AS(0x80838C00,MH3_HBMFontTexture,"HBMFontTexture");
extern "C" void MH3_SystemUIVertexFormat(CpuContext*cpu){if(uint8_t(cpu->gpr[4])>8)throw std::runtime_error("Layout vertex format has too many coordinates");format={cpu->gpr[3]!=0,uint8_t(cpu->gpr[4])};}
REGISTER_NATIVE_FUNCTION_AS(0x80535E6C,MH3_SystemUIVertexFormat,"SystemUIVertexFormat");
REGISTER_NATIVE_FUNCTION_AS(0x8082A360,MH3_SystemUIVertexFormat,"HBMVertexFormat");
extern "C" void MH3_HBMFontVertexFormat(CpuContext*){format={true,1};}
REGISTER_NATIVE_FUNCTION_AS(0x80838E70,MH3_HBMFontVertexFormat,"HBMFontVertexFormat");
extern "C" void MH3_SystemUIQuad(CpuContext*cpu){Quad(cpu,-1);}
REGISTER_NATIVE_FUNCTION_AS(0x80535F50,MH3_SystemUIQuad,"SystemUIQuad");
extern "C" void MH3_HBMQuad(CpuContext*cpu){Quad(cpu,1);}
REGISTER_NATIVE_FUNCTION_AS(0x8082A580,MH3_HBMQuad,"HBMQuad");
extern "C" void MH3_SystemUIPane(CpuContext*cpu){LoadPane(cpu->gpr[3],cpu->gpr[4],0);}
REGISTER_NATIVE_FUNCTION_AS(0x8052B654,MH3_SystemUIPane,"SystemUIPane");
extern "C" void MH3_SystemUITextPane(CpuContext*cpu){LoadPane(cpu->gpr[3],cpu->gpr[4],1);}
REGISTER_NATIVE_FUNCTION_AS(0x8052E2B8,MH3_SystemUITextPane,"SystemUITextPane");
extern "C" void MH3_HBMPane(CpuContext*cpu){LoadPane(cpu->gpr[3],cpu->gpr[4],2);}
REGISTER_NATIVE_FUNCTION_AS(0x808313B0,MH3_HBMPane,"HBMPane");
extern "C" void MH3_SystemUIIndirectMatrix(CpuContext*cpu){
    const unsigned index=cpu->gpr[3];if(index<1||index>3)throw std::runtime_error("Layout indirect matrix selector unsupported");
    float values[6];Floats(cpu->gpr[4],values,6);auto material=GetNativeMaterialState();
    if(!mh3_system_ui_indirect_matrix(values,&material.indirect_matrix[index-1]))throw std::runtime_error("Layout indirect matrix is unrepresentable");SetNativeMaterial(material);
}
REGISTER_NATIVE_FUNCTION_AS(0x80531010,MH3_SystemUIIndirectMatrix,"SystemUIIndirectMatrix");
REGISTER_NATIVE_FUNCTION_AS(0x8082C370,MH3_SystemUIIndirectMatrix,"HBMIndirectMatrix");
extern "C" void MH3_HBMTPL(CpuContext*cpu){ConstructTPL(cpu,cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],true);}
REGISTER_NATIVE_FUNCTION_AS(0x8082ADC0,MH3_HBMTPL,"HBMTPL");
extern "C" void MH3_HBMTextureGet(CpuContext*cpu){CopyTexture(cpu->gpr[4],TextureSlot(cpu->gpr[3],cpu->gpr[5]),false,true);}
REGISTER_NATIVE_FUNCTION_AS(0x8082EB40,MH3_HBMTextureGet,"HBMTextureGet");
extern "C" void MH3_HBMTextureSet(CpuContext*cpu){CopyTexture(TextureSlot(cpu->gpr[3],cpu->gpr[4]),cpu->gpr[5],false,true);}
REGISTER_NATIVE_FUNCTION_AS(0x8082EC50,MH3_HBMTextureSet,"HBMTextureSet");
extern "C" void MH3_HBMTextureNoWrap(CpuContext*cpu){CopyTexture(TextureSlot(cpu->gpr[3],cpu->gpr[4]),cpu->gpr[5],true,true);}
REGISTER_NATIVE_FUNCTION_AS(0x8082ECB0,MH3_HBMTextureNoWrap,"HBMTextureNoWrap");
extern "C" void MH3_HBMTPLNoWrap(CpuContext*cpu){const auto object=TextureSlot(cpu->gpr[3],cpu->gpr[4]),tpl=cpu->gpr[5];const auto old=ReadNativeTextureObject(object);ConstructTPL(cpu,object,tpl,0,true);auto b=ReadNativeTextureObject(object);b.sampler.wrap_s=old.sampler.wrap_s;b.sampler.wrap_t=old.sampler.wrap_t;StoreNativeTextureObject(object,b);Memory::Write32(object+16,palette_headers.at(object));}
REGISTER_NATIVE_FUNCTION_AS(0x8082EBC0,MH3_HBMTPLNoWrap,"HBMTPLNoWrap");
extern "C" void MH3_HBMTextureSize(CpuContext*cpu){const auto output=cpu->gpr[3],material=cpu->gpr[4],slot=cpu->gpr[5];float width=0,height=0;if(slot<Memory::Read32(material+0x50)>>28){const auto binding=ReadNativeTextureObject(TextureSlot(material,slot));width=binding.source.width;height=binding.source.height;}Memory::WriteFloat32(output,width);Memory::WriteFloat32(output+4,height);}
REGISTER_NATIVE_FUNCTION_AS(0x808300B0,MH3_HBMTextureSize,"HBMTextureSize");
extern "C" void MH3_SystemUITextureImport(CpuContext*cpu) {
    const auto info=cpu->gpr[3],object=cpu->gpr[4];mh3_native_texture_binding binding{};
    Checked(mh3_system_ui_texture_info_read(Bytes(info,28),ReadNativeTextureGeneration(Memory::Read32(info)),&binding));
    if(binding.source.indexed)binding.tlut_id=ReadNativeTextureObject(object).tlut_id;
    binding.source.resource_address=info;binding.source.generation=Produced(binding.source.payload_address);
    StoreNativeTextureObject(object,binding);palette_headers[object]=0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80533C50,MH3_SystemUITextureImport,"SystemUITextureImport");
extern "C" void MH3_SystemUIPaletteImport(CpuContext*cpu) {
    const auto info=cpu->gpr[3],object=cpu->gpr[4];mh3_native_palette_source palette{};
    palette.resource_address=info;palette.payload_address=Memory::Read32(info+4);
    palette.format=(Memory::Read32(info+24)>>13)&3;palette.entries=Memory::Read16(info+22);
    palette.generation=Produced(palette.payload_address);StoreNativePaletteObject(object,palette);
}
REGISTER_NATIVE_FUNCTION_AS(0x80533D3C,MH3_SystemUIPaletteImport,"SystemUIPaletteImport");
extern "C" void MH3_SystemUITextureExport(CpuContext*cpu) {
    const auto binding=ReadNativeTextureObject(cpu->gpr[4]);Checked(mh3_system_ui_texture_info_store(Mutable(cpu->gpr[3],28),&binding));
}
REGISTER_NATIVE_FUNCTION_AS(0x80533D58,MH3_SystemUITextureExport,"SystemUITextureExport");
extern "C" void MH3_SystemUISaveDraw(CpuContext*cpu){SaveDraw(cpu,0);}
REGISTER_NATIVE_FUNCTION_AS(0x8055DC5C,MH3_SystemUISaveDraw,"SystemUISaveDraw");
extern "C" void MH3_SystemUISaveAlways(CpuContext*cpu){SaveDraw(cpu,1);}
REGISTER_NATIVE_FUNCTION_AS(0x80564CEC,MH3_SystemUISaveAlways,"SystemUISaveAlways");
extern "C" void MH3_SystemUISaveDepth(CpuContext*cpu){SaveDraw(cpu,2);}
REGISTER_NATIVE_FUNCTION_AS(0x80565238,MH3_SystemUISaveDepth,"SystemUISaveDepth");
namespace {
uint32_t LayoutMaterial(CpuContext*cpu,bool hbm){
    GuestFrame frame(cpu,0x80);GuestCall call{cpu};const auto self=frame.Arg(3),a=Memory::Read32(self+(hbm?0x4c:0x38));
    uint32_t size;Checked(mh3_system_ui_layout_size(a,hbm,&size));const auto storage=Memory::Read32(self+(hbm?0x58:0x40));
    uint8_t kc[8],ka[8];for(unsigned i=0;i<8;++i){kc[i]=Memory::Read32((hbm?0x808464f8:0x8064cd10)+i*4);ka[i]=Memory::Read32((hbm?0x80846518:0x8064cd30)+i*4);}
    auto material=GetNativeMaterialState();mh3_system_ui_layout next{};
    Checked(mh3_system_ui_layout_material(&material,Bytes(self,hbm?0x58:0x40),Bytes(storage,size),hbm,frame.Arg(4)!=0,uint8_t(frame.Arg(5)),kc,ka,&next));
    for(unsigned i=0;i<10;++i){
        const uint32_t selector=30+i*3;
        if(next.texture_matrices[i]&&i<next.texture_srt_count){call(hbm?0x8082c250:0x80530f08,{frame[0x10],storage+next.srt_offset+i*20});Floats(frame[0x10],layout_matrices[i],12);SetNativeTextureMatrix(selector,layout_matrices[i],false);}
        else if(!GetNativeTextureMatrix(selector,layout_matrices[i])){for(unsigned j=0;j<next.texgen_count;++j)if(next.texgen_selector[j]==selector)throw std::runtime_error("Layout references an unpublished texture matrix");}
    }
    for(unsigned i=0;i<3;++i)if(next.indirect_matrices[i]&&i<next.indirect_srt_count){
        const uint32_t record=storage+next.indirect_srt_offset+i*20;const float angle=Memory::ReadFloat32(record+8)*Memory::ReadFloat32(hbm?0x808448ac:0x8079fa74);
        call(hbm?0x808367a0:0x8050151c,{}, {angle});const double cosine=cpu->fpr[1].d;
        call(hbm?0x80836720:0x805014b4,{}, {angle});const double sine=cpu->fpr[1].d;
        const float sx=Memory::ReadFloat32(record+12),sy=Memory::ReadFloat32(record+16);
        const float values[6]={float(cosine*sx),float(-sine*sy),Memory::ReadFloat32(record),float(sine*sx),float(cosine*sy),Memory::ReadFloat32(record+4)};
        if(!mh3_system_ui_indirect_matrix(values,&material.indirect_matrix[i]))throw std::runtime_error("Layout indirect SRT exceeds coefficient domain");
    }
    uint32_t small_palette=0,large_palette=16;
    for(unsigned i=0;i<next.texture_count;++i){const uint32_t object=storage+i*next.texture_stride;mh3_native_texture_binding binding{};
        if(hbm)binding=ReadNativeTextureObject(object);
        else{const uint32_t payload=Memory::Read32(object);Checked(mh3_system_ui_texture_info_read(Bytes(object,28),ReadNativeTextureGeneration(payload),&binding));
            binding.source.resource_address=object;if(!binding.source.generation)throw std::runtime_error("Layout texture has no explicit resource producer");}
        binding.map_id=i;
        if(binding.source.indexed&&(!hbm||(binding.source.format<10&&Memory::Read32(object+16)))){mh3_native_palette_source palette{};
            if(hbm){const auto header=Memory::Read32(object+16);if(!header)throw std::runtime_error("HBM indexed texture lacks authored palette user data");
                palette.resource_address=header;palette.entries=Memory::Read16(header);palette.format=Memory::Read32(header+4);palette.payload_address=Memory::Read32(header+8);}
            else{palette.resource_address=object;palette.payload_address=Memory::Read32(object+4);palette.entries=Memory::Read16(object+22);palette.format=(Memory::Read32(object+24)>>13)&3;}
            palette.generation=ReadNativeTextureGeneration(palette.payload_address);if(!palette.generation)throw std::runtime_error("Layout palette has no explicit resource producer");
            binding.tlut_id=binding.source.format==10?large_palette++:small_palette++;BindNativePalette(binding.tlut_id,palette);
        }BindNativeTexture(binding);
    }
    layout=next;layout_coordinates=true;SetNativeMaterial(material);return next.modulation&&next.vertex_colors;
}
}
extern "C" void MH3_SystemUILayoutMaterial(CpuContext*cpu){cpu->gpr[3]=LayoutMaterial(cpu,false);}
REGISTER_NATIVE_FUNCTION_AS(0x80532BB0,MH3_SystemUILayoutMaterial,"SystemUILayoutMaterial");
extern "C" void MH3_HBMLayoutMaterial(CpuContext*cpu){cpu->gpr[3]=LayoutMaterial(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x8082F0B0,MH3_HBMLayoutMaterial,"HBMLayoutMaterial");
extern "C" void MH3_HBMSimpleQuad(CpuContext*cpu){
    if(format.coordinates!=1 || format.colors!=(cpu->gpr[7]!=0))throw std::runtime_error("HBM simple quad format disagrees with emitted attributes");
    if(!NativeGraphicsFrameShouldRender())return;
    float left_top[2],right_bottom[2],uv0[2],uv1[2];Floats(cpu->gpr[3],left_top,2);Floats(cpu->gpr[4],right_bottom,2);Floats(cpu->gpr[5],uv0,2);Floats(cpu->gpr[6],uv1,2);
    float origin[3]={left_top[0],left_top[1],0},size[2]={right_bottom[0]-left_top[0],right_bottom[1]-left_top[1]},uv[8][4][2]={};uint8_t colors[4][4];Colors(cpu->gpr[7],colors);
    for(unsigned i=0;i<4;++i){uv[0][i][0]=(i&1)?uv1[0]:uv0[0];uv[0][i][1]=(i&2)?uv1[1]:uv0[1];}
    mh3_system_ui_vertex vertices[4];mh3_system_ui_quad(vertices,origin,size,1,uv,1,colors);Submit(vertices,4);
}
REGISTER_NATIVE_FUNCTION_AS(0x8082A450,MH3_HBMSimpleQuad,"HBMSimpleQuad");
namespace {
void Identity(){float m[12];mh3_draw_identity_position(m);SetNativePositionMatrix(m);}
void Ortho(float top,float bottom,float left,float right,float near_z,float far_z){float m[16];if(!mh3_draw_ortho(m,top,bottom,left,right,near_z,far_z))throw std::runtime_error("System UI orthographic volume is empty");SetNativeProjection(m,false);}
void Pixel(mh3_native_material_state&m,unsigned field,uint32_t value){m.pixel.known|=UINT64_C(1)<<field;m.pixel.values[field]=value;}
void AlphaAlways(mh3_native_material_state&m,unsigned operation=0){Pixel(m,MH3_PIXEL_ALPHA_COMPARE0,7);Pixel(m,MH3_PIXEL_ALPHA_COMPARE1,7);Pixel(m,MH3_PIXEL_ALPHA_REFERENCE0,0);Pixel(m,MH3_PIXEL_ALPHA_REFERENCE1,0);Pixel(m,MH3_PIXEL_ALPHA_OPERATION,operation);}
void FadeSetup(){auto m=GetNativeMaterialState();mh3_system_ui_fade_material(&m);SetNativeMaterial(m);Identity();format={false,0};layout_coordinates=false;}
void Fade(uint32_t self,unsigned timer,unsigned duration,unsigned color){
    FadeSetup();const int32_t divisor=Memory::Read32(self+duration);if(!divisor)throw std::runtime_error("HBM fade has zero duration");
    const uint8_t alpha=(int64_t(int32_t(Memory::Read32(self+timer)))*255)/divisor;auto m=GetNativeMaterialState();
    for(unsigned i=0;i<3;++i)m.color[1].rgba[i]=Memory::Read8(self+color)?Memory::Read8(self+color+1+i):0;m.color[1].rgba[3]=alpha;m.color[1].known|=15;SetNativeMaterial(m);
    const float origin[3]={-1000,-1000,0},size[2]={2000,2000};mh3_system_ui_vertex vertices[4];mh3_system_ui_quad(vertices,origin,size,1,nullptr,0,nullptr);Submit(vertices,4);
}
void DebugQuad(CpuContext*cpu,unsigned mode,bool hbm=false){
    auto m=GetNativeMaterialState();mh3_system_ui_debug_material(&m,mode!=2);SetNativeMaterial(m);layout_coordinates=false;format={true,0};
    float left=cpu->fpr[1].d,top=cpu->fpr[2].d,right=cpu->fpr[3].d,bottom=cpu->fpr[4].d,depth=cpu->fpr[5].d;
    uint8_t color[4];for(unsigned i=0;i<4;++i)color[i]=Memory::Read8(cpu->gpr[mode?4:3]+i);
    if(mode==0){const float scale=cpu->fpr[6].d,cx=left+(right-left)*.5f,cy=top+(bottom-top)*.5f;
        const float matrix[12]={scale,0,0,cx,0,scale,0,cy,0,0,scale,depth};SetNativePositionMatrix(matrix);left-=cx;right-=cx;top-=cy;bottom-=cy;depth=0;
    }else Identity();
    if(mode==2){
        if(hbm){const float matrix[12]={1,0,0,0,0,1,0,0,0,0,1,Memory::ReadFloat32(0x80844498)};SetNativePositionMatrix(matrix);}
        mh3_system_ui_vertex points[2]{};points[0].position[0]=left;points[0].position[1]=top;points[0].position[2]=depth;points[1].position[0]=right;points[1].position[1]=bottom;points[1].position[2]=depth;
        for(auto&point:points){std::copy(color,color+4,point.color);for(auto&coord:point.coordinate)coord[2]=1;}
        if(NativeGraphicsFrameShouldRender()){AuroraNativeMaterialGeometry geometry{};if(aurora_allocate_native_material_geometry(4,6,&geometry)){Checked(mh3_system_ui_lines(&GetNativeDrawState(),points,2,float(uint8_t(cpu->gpr[3]))/6,&geometry));SubmitNativeMaterialGeometry(geometry);}}
    }else{
        if(mode==1){const float half=float(cpu->gpr[3])/6*.5f;if(left==right){left-=half;right+=half;}else{top-=half;bottom+=half;}}
        const float origin[3]={left,top,depth},size[2]={right-left,bottom-top};uint8_t colors[4][4];for(auto&c:colors)std::copy(color,color+4,c);
        mh3_system_ui_vertex vertices[4];mh3_system_ui_quad(vertices,origin,size,1,nullptr,0,colors);
        // Authored debug quads walk left-top,left-bottom,right-bottom,right-top.
        std::swap(vertices[1],vertices[3]);Submit(vertices,4);
    }
    if(mode==0)Identity();
}
}
extern "C" void MH3_HBMFadeSetup(CpuContext*){FadeSetup();}
REGISTER_NATIVE_FUNCTION_AS(0x8081ED60,MH3_HBMFadeSetup,"HBMFadeSetup");
extern "C" void MH3_HBMBlackFader(CpuContext*cpu){Fade(cpu->gpr[3],0,4,12);}
REGISTER_NATIVE_FUNCTION_AS(0x80828410,MH3_HBMBlackFader,"HBMBlackFader");
extern "C" void MH3_HBMHomeButton(CpuContext*cpu){GuestFrame frame(cpu,0x10);GuestCall call{cpu};const auto self=frame.Arg(3);FadeSetup();call.Virtual(Memory::Read32(self+0x238),0x28,{self+600});if(!Memory::Read32(Memory::Read32(self+4)+0x20))for(unsigned i=0;i<4;++i)call.Virtual(Memory::Read32(self+0x248-i*4),0x28,{self+600});Fade(self,0x454,0x458,0x460);}
REGISTER_NATIVE_FUNCTION_AS(0x80822E10,MH3_HBMHomeButton,"HBMHomeButton");
extern "C" void MH3_HBMDraw(CpuContext*cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};if(!call(0x80040848))return;call(0x8003fd28);
    const bool wide=call(0x80040424)!=0;const auto mode=Memory::Read32(0x80796c28);const float width=Memory::Read16(mode+14);
    const float height=float(double(448.0f/456.0f)*double(float(456.0f*.5f))),half=(wide?416.0f:304.0f)*(width/(wide?686.0f:670.0f));
    Ortho(height,-height,-half,half,0,500);auto raster=GetNativeDrawState().raster;raster.cull_mode=0;SetNativeRaster(raster);call(0x8081eaf0);
    auto m=GetNativeMaterialState();AlphaAlways(m,1);m.indirect_count=0;m.cull_mode=0;Pixel(m,MH3_PIXEL_DEPTH_TEST,1);Pixel(m,MH3_PIXEL_DEPTH_COMPARE,7);Pixel(m,MH3_PIXEL_DEPTH_WRITE,0);
    for(unsigned i=0;i<4;++i){m.tev[i].raster_swap=m.tev[i].texture_swap=0;m.tev[i].known|=(UINT64_C(1)<<MH3_TEV_RASTER_SWAP)|(UINT64_C(1)<<MH3_TEV_TEXTURE_SWAP);}SetNativeMaterial(m);
}
REGISTER_NATIVE_FUNCTION_AS(0x8081A49C,MH3_HBMDraw,"HBMDraw");
extern "C" void MH3_SystemUIDebugQuad(CpuContext*cpu){DebugQuad(cpu,0);}
REGISTER_NATIVE_FUNCTION_AS(0x8056A648,MH3_SystemUIDebugQuad,"SystemUIDebugQuad");
extern "C" void MH3_SystemUIDebugStroke(CpuContext*cpu){DebugQuad(cpu,1);}
REGISTER_NATIVE_FUNCTION_AS(0x8056A9BC,MH3_SystemUIDebugStroke,"SystemUIDebugStroke");
extern "C" void MH3_SystemUIDebugLine(CpuContext*cpu){DebugQuad(cpu,2);}
REGISTER_NATIVE_FUNCTION_AS(0x8056AC5C,MH3_SystemUIDebugLine,"SystemUIDebugLine");
extern "C" void MH3_HBMDebugLine(CpuContext*cpu){DebugQuad(cpu,2,true);}
REGISTER_NATIVE_FUNCTION_AS(0x8081ACD0,MH3_HBMDebugLine,"HBMDebugLine");
extern "C" void MH3_HBMOutline(CpuContext*cpu){
    auto m=GetNativeMaterialState();const uint32_t inherited_cull=m.cull_mode;mh3_system_ui_debug_material(&m,1);m.cull_mode=inherited_cull;m.indirect_count=0;
    m.channel[0].color=m.channel[0].alpha=(mh3_pack_channel_control){0,0,0,0,2,0};m.channel[0].flags|=MH3_CHANNEL_MATERIAL_COLOR|MH3_CHANNEL_MATERIAL_ALPHA;
    uint8_t color[4];for(unsigned i=0;i<4;++i){color[i]=Memory::Read8(cpu->gpr[5]+i);m.channel[0].material_color[i]=color[i];m.swaps[0].rgba[i]=i;}m.swaps[0].known|=15;for(unsigned f=MH3_TEV_INDIRECT_STAGE;f<MH3_TEV_FIELD_COUNT;++f)m.tev[0].known|=UINT64_C(1)<<f;
    m.tev[0].raster_swap=m.tev[0].texture_swap=m.tev[0].indirect_stage=m.tev[0].indirect_format=m.tev[0].indirect_bias=m.tev[0].indirect_alpha=m.tev[0].indirect_matrix=m.tev[0].indirect_wrap_s=m.tev[0].indirect_wrap_t=m.tev[0].indirect_original_lod=m.tev[0].indirect_add_previous=0;m.tev[0].known|=(UINT64_C(1)<<MH3_TEV_RASTER_SWAP)|(UINT64_C(1)<<MH3_TEV_TEXTURE_SWAP);AlphaAlways(m);SetNativeMaterial(m);format={false,0};layout_coordinates=false;
    if(!NativeGraphicsFrameShouldRender())return;float origin[3]={0,0,0},size[2];Floats(cpu->gpr[3],origin,2);Floats(cpu->gpr[4],size,2);
    mh3_system_ui_vertex points[5];mh3_system_ui_quad(points,origin,size,1,nullptr,0,nullptr);points[4]=points[0];
    AuroraNativeMaterialGeometry geometry{};if(aurora_allocate_native_material_geometry(16,24,&geometry)){Checked(mh3_system_ui_lines(&GetNativeDrawState(),points,5,1,&geometry));SubmitNativeMaterialGeometry(geometry);}
}
REGISTER_NATIVE_FUNCTION_AS(0x8082AC20,MH3_HBMOutline,"HBMOutline");
extern "C" void MH3_SystemUITextureInfoTPL(CpuContext*cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};const auto info=frame.Arg(3),tpl=frame.Arg(4),index=frame.Arg(5);
    if(Memory::Read32(tpl+8)<0x80000000u)call(0x804e4c30,{tpl});const auto desc=call(0x804e4d50,{tpl,index}),image=Memory::Read32(desc),palette=Memory::Read32(desc+4);
    const auto flags=Memory::Read32(info+24);Memory::Write32(info,Memory::Read32(image+8));Memory::Write16(info+8,Memory::Read16(image+2));Memory::Write16(info+10,Memory::Read16(image));
    const auto format_bits=Memory::Read32(image+4)<<28;Memory::Write32(info+24,format_bits|(flags&0x0fff9fffu)|(palette?((Memory::Read32(palette+4)&3)<<13):0));
    Memory::Write32(info+4,palette?Memory::Read32(palette+8):0);Memory::Write16(info+22,palette?Memory::Read16(palette):0);
    Produced(Memory::Read32(image+8));if(palette)Produced(Memory::Read32(palette+8));
}
REGISTER_NATIVE_FUNCTION_AS(0x80533EAC,MH3_SystemUITextureInfoTPL,"SystemUITextureInfoTPL");
extern "C" void MH3_HBMMaterialConstruct(CpuContext*cpu){
    const auto self=cpu->gpr[3],resource=cpu->gpr[4],blocks=cpu->gpr[5];
    {GuestFrame frame(cpu,0x50);GuestCall call{cpu};Memory::Write32(self,0x80846538);Memory::Write32(self+24,0);Memory::Write32(self+28,self+28);Memory::Write32(self+32,self+28);
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)Memory::Write16(self+0x24+i*8+j*2,i?255:0);
        for(unsigned i=0;i<4;++i)Memory::Write32(self+0x3c+i*4,0xffffffff);Memory::Write32(self+0x4c,Memory::Read32(self+0x4c)&31);Memory::Write32(self+0x50,Memory::Read32(self+0x50)&31);Memory::Write8(self+0x54,0);Memory::Write32(self+0x58,0);
        std::memcpy(Memory::GetPointer(self+0x24,40),Memory::GetPointer(resource+20,40),40);
        for(unsigned i=0;i<20;++i){const auto c=Memory::Read8(resource+i);Memory::Write8(self+4+i,c);if(!c){for(unsigned j=i+1;j<20;++j)Memory::Write8(self+4+j,0);break;}}
        mh3_system_ui_capacity capacity{};Checked(mh3_system_ui_hbm_capacity(Bytes(resource,64),&capacity));
        Memory::Write32(frame[8],capacity.channel);Memory::Write32(frame[12],capacity.material_color);Memory::Write32(frame[16],capacity.alpha);Memory::Write32(frame[20],capacity.blend);
        call(0x8082d9f0,{self,capacity.texture_count,capacity.srt_count,capacity.texgen_count,capacity.tev_count,capacity.swap,capacity.indirect_count,capacity.indirect_srt_count});
        const auto storage=Memory::Read32(self+0x58);if(storage){
            uint32_t size;Checked(mh3_system_ui_layout_size(Memory::Read32(self+0x4c),1,&size));const uint32_t flags=capacity.resource_flags;
            const uint32_t source_size=64+(flags&15)*4+((flags>>4)&15)*20+((flags>>8)&15)*4+(capacity.channel+capacity.material_color+capacity.swap+capacity.alpha+capacity.blend)*4+((flags>>13)&3)*20+((flags>>15)&7)*4+((flags>>18)&31)*16;
            Checked(mh3_system_ui_hbm_construct(Mutable(self,0x5c),Mutable(storage,size),Bytes(resource,source_size),&capacity));
            for(unsigned i=0;i<capacity.texture_count;++i){const auto list=Memory::Read32(blocks),entry=list+12+Memory::Read32(list+12+Memory::Read16(resource+64+i*4)*8);
                const auto tpl=call.Virtual(Memory::Read32(blocks+12),12,{0x74696d67,entry,0});ConstructTPL(cpu,storage+i*32,tpl,0,true);auto binding=ReadNativeTextureObject(storage+i*32);
                binding.sampler.wrap_s=Memory::Read8(resource+66+i*4);binding.sampler.wrap_t=Memory::Read8(resource+67+i*4);StoreNativeTextureObject(storage+i*32,binding);Memory::Write32(storage+i*32+16,palette_headers.at(storage+i*32));}
        }
    }cpu->gpr[3]=self;
}
REGISTER_NATIVE_FUNCTION_AS(0x8082C720,MH3_HBMMaterialConstruct,"HBMMaterialConstruct");
extern "C" void MH3_SystemUIPanelBegin(CpuContext*cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};const auto self=frame.Arg(3);float bounds[4],panel[4],projection[16],viewport[6],record[7];int32_t scissor[4];Floats(frame.Arg(4),bounds,4);Floats(self+0x88,panel,4);
    const auto saved=GetNativeDrawState();Memory::Write32(self+0xc4,0);for(unsigned i=0;i<6;++i)Memory::WriteFloat32(self+0x54+i*4,saved.viewport[i]);
    mh3_system_ui_projection_record(&saved,record);for(unsigned i=0;i<7;++i)Memory::WriteFloat32(self+0x6c+i*4,record[i]);
    if(!mh3_system_ui_panel_view(bounds,panel,Memory::ReadFloat32(self+0x98),saved.viewport,projection,viewport,scissor))throw std::runtime_error("Save UI panel has invalid crop volume");
    SetNativeProjection(projection,false);SetNativeViewport(viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5]);SetNativeScissor(scissor[0],scissor[1],scissor[2],scissor[3]);Identity();
    Memory::Write32(self+0xa0,0);Memory::Write32(self+0xb8,Memory::Read32(self+0x30));if(Memory::Read8(self+0x105)){Memory::Write8(self+0x105,0);call(Memory::Read32(Memory::Read32(self+0x4c)+0x90),{self});call(Memory::Read32(Memory::Read32(self+0x4c)+0x94),{self});}
}
REGISTER_NATIVE_FUNCTION_AS(0x8055B950,MH3_SystemUIPanelBegin,"SystemUIPanelBegin");
extern "C" void MH3_SystemUIPanelEnd(CpuContext*cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};const auto self=frame.Arg(3);float viewport[6];Floats(self+0x54,viewport,6);SetNativeViewport(viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5]);
    int32_t scissor[4];for(unsigned i=0;i<4;++i){if(viewport[i]<0||viewport[i]>2147483647.0)throw std::runtime_error("Save panel restored scissor exceeds logical coordinates");scissor[i]=uint32_t(viewport[i]);}SetNativeScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
    const double previous=Memory::ReadFloat32(self+0x30);Memory::Write32(self+0xb4,Memory::Read32(self+0x2c));call(Memory::Read32(Memory::Read32(self+0x4c)+0x28),{self});Memory::WriteFloat32(self+0xb8,float(double(float(double(Memory::ReadFloat32(self+0xb8))-previous))+cpu->fpr[1].d));
}
REGISTER_NATIVE_FUNCTION_AS(0x8055BB38,MH3_SystemUIPanelEnd,"SystemUIPanelEnd");
extern "C" void MH3_SystemUIPanelProjection(CpuContext*cpu){
    GuestFrame frame(cpu,0x30);GuestCall call{cpu};const auto self=frame.Arg(3);call(0x8052c718,{frame[0x10],Memory::Read32(self+4)});const bool full=!Memory::Read8(self+0x89);
    if(full){for(unsigned i=0;i<4;++i)Memory::WriteFloat32(frame[0x10+i*4],0);call(0x8055fea8,{frame[0x10]});}
    float bounds[4];Floats(frame[0x10],bounds,4);Ortho(full?bounds[3]:bounds[1],full?bounds[1]:bounds[3],bounds[0],bounds[2],-1,1);
}
REGISTER_NATIVE_FUNCTION_AS(0x8055CB18,MH3_SystemUIPanelProjection,"SystemUIPanelProjection");
extern "C" void MH3_SystemUIPanelDraw(CpuContext*cpu){
    GuestFrame frame(cpu,0x30);GuestCall call{cpu};const auto self=frame.Arg(3),draw_info=frame.Arg(4);const auto selected=mh3_system_ui_panel_selection(Memory::Read32(self+0x1d0),Memory::Read32(self+0x1e4),Memory::Read32(self+0x1e8),Memory::Read32(self+0x1d8));
    auto inactive=[&](){return call(Memory::Read32(Memory::Read32(self+0x18)+0x14),{self+0x18})==0;};
    if(inactive())for(unsigned i=0;i<20;++i)if(i==selected||call.Virtual(Memory::Read32(self+0xe0+i*4),0x24)==2){const auto pane=Memory::Read32(Memory::Read32(self+0x38+i*4)+0x9c);Memory::Write8(pane+0xbb,Memory::Read8(pane+0xbb)&~1);}
    const auto pane=Memory::Read32(Memory::Read32(self+0xd8)+0x9c);call(0x8052abd8,{frame[0x10],pane,draw_info});float bounds[4],matrix[12],left[3],right[3];Floats(frame[0x10],bounds,4);Floats(pane+0x84,matrix,12);
    const float start[3]={bounds[0],bounds[1],0},end[3]={bounds[2],bounds[3],0};mh3_system_ui_transform(left,matrix,start);mh3_system_ui_transform(right,matrix,end);
    int32_t saved[4];if(!GetNativeScissor(saved))throw std::runtime_error("Panel crop has no published scissor to restore");
    const float width=Memory::ReadFloat32(self+500),scale=Memory::ReadFloat32(0x8079fb64)/width;
    float x=left[0]+width*Memory::ReadFloat32(0x8079fb74);x=x+x*(scale-Memory::ReadFloat32(0x8079fb68));const float crop_width=scale*(right[0]-left[0]);
    if(!std::isfinite(x)||!std::isfinite(crop_width)||x<0||crop_width<0||x>2147483647.0||crop_width>2147483647.0)throw std::runtime_error("Panel crop is outside supported logical coordinates");
    SetNativeScissor(int32_t(x),saved[1],int32_t(crop_width),saved[3]);call.Virtual(Memory::Read32(Memory::Read32(self+0xdc)+0x9c),0x14,{draw_info});
    if(inactive()){const float width2=crop_width+x-saved[0];if(width2<0||width2>2147483647.0)throw std::runtime_error("Panel second crop is outside logical coordinates");SetNativeScissor(saved[0],saved[1],int32_t(width2),saved[3]);
        for(unsigned i=0;i<20;++i)if(i==selected||call.Virtual(Memory::Read32(self+0xe0+i*4),0x24)==2){const auto child=Memory::Read32(Memory::Read32(self+0x38+i*4)+0x9c);Memory::Write8(child+0xbb,Memory::Read8(child+0xbb)|1);call.Virtual(child,0x14,{draw_info});}}
    SetNativeScissor(saved[0],saved[1],saved[2],saved[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8055319C,MH3_SystemUIPanelDraw,"SystemUIPanelDraw");
namespace {
void TPLRecord(GuestCall&call,uint32_t tpl,uint32_t index,uint32_t record,bool linear){
    const auto descriptor=call(0x804e4d50,{tpl,index}),header=Memory::Read32(descriptor);mh3_ui_texture_header image{};Checked(mh3_ui_texture_header_read(Bytes(header,36),&image));
    Memory::Write32(record+0x24,image.payload_address);Memory::Write16(record+0x28,image.width);Memory::Write16(record+0x2a,image.height);Memory::Write32(record+0x2c,image.format);Memory::Write32(record+0x30,image.wrap_s);Memory::Write32(record+0x34,image.wrap_t);
    mh3_native_texture_binding binding{};binding.source.resource_address=header;binding.source.payload_address=image.payload_address;binding.source.width=image.width;binding.source.height=image.height;binding.source.format=image.format;binding.source.levels=1;binding.source.indexed=image.format>=8&&image.format<=10;binding.source.generation=Produced(image.payload_address);
    binding.sampler.wrap_s=image.wrap_s;binding.sampler.wrap_t=image.wrap_t;binding.sampler.min_filter=binding.sampler.mag_filter=linear;
    StoreNativeTextureObject(record+4,binding);Memory::Write8(record,1);
}
uint32_t TPLArray(CpuContext*cpu,bool global){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};const auto tpl=frame.Arg(3),destination=frame.Arg(4);call(0x804e4c30,{tpl});const uint32_t count=Memory::Read32(tpl+4);uint32_t produced=0;
    if(!global){for(unsigned i=0;i<count;++i){TPLRecord(call,tpl,i,destination+i*56,true);++produced;}}
    else {uint32_t record;if(destination==255)record=0x8065b3cc;else if(destination==254)record=0x8065b404;else if(destination>31)return 0;else record=0x8065ab1c+destination*56;
        for(unsigned i=0;i<count;++i){if(destination<254&&destination+i>31)break;if(destination>=254&&Memory::Read8(record))break;
            if(!Memory::Read8(record)){if(destination==5&&i==2)record=0x8065ad84;TPLRecord(call,tpl,i,record,false);++produced;if(destination==255)break;}record+=56;}}
    return produced;
}
}
extern "C" void MH3_UITPLArray(CpuContext*cpu){cpu->gpr[3]=TPLArray(cpu,false);}
REGISTER_NATIVE_FUNCTION_AS(0x800D0324,MH3_UITPLArray,"UITPLArray");
extern "C" void MH3_UITPLGlobalArray(CpuContext*cpu){cpu->gpr[3]=TPLArray(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x800D042C,MH3_UITPLGlobalArray,"UITPLGlobalArray");
extern "C" void MH3_SystemUITextDraw(CpuContext*cpu){
    GuestFrame frame(cpu,0x180);GuestCall call{cpu};const auto self=frame.Arg(3),text=Memory::Read32(self+0xd8),font=Memory::Read32(self+0xe4),material=Memory::Read32(self+0x28);
    if(!text||!font||!material)return;call.Virtual(self,0x70,{frame.Arg(4)});
    const auto writer=frame[0x30],line_writer=frame[0xa0],rect=frame[0x110],line_rect=frame[0x120],first=frame[0x140],second=frame[0x144],mapping0=frame[0x148],mapping1=frame[0x14c],break_flag=frame[0x150];
    call(0x80505170,{writer});call(0x8052e33c,{rect,self,writer});
    for(unsigned n=0;n<2;++n){for(unsigned i=0;i<4;++i)Memory::Write8((n?second:first)+i,Memory::Read8(self+0xdc+n*4+i));Memory::Write8((n?second:first)+3,uint32_t(Memory::Read8((n?second:first)+3))*Memory::Read8(self+0xb9)/255);}
    Memory::Write32(writer+32,Memory::Read32(first)!=Memory::Read32(second)?2:0);call(0x80504c20,{writer});call(0x8052dbdc,{writer,first,second});
    call(0x8052d05c,{mapping0,material+16});call(0x8052d05c,{mapping1,material+24});call(0x80503bb4,{writer,mapping0,mapping1});call(0x80503c38,{writer});
    auto m=GetNativeMaterialState();AlphaAlways(m);SetNativeMaterial(m);
    unsigned mode=(Memory::Read8(self+0x101)>>5)&3;if(mode==0)mode=Memory::Read8(self+0x100)%3+1;const float alignment=mode==1?0:mode==2?.5f:1;
    const float left=Memory::ReadFloat32(rect),width=Memory::ReadFloat32(rect+8)-left;Memory::WriteFloat32(writer+44,left);Memory::WriteFloat32(writer+48,-Memory::ReadFloat32(rect+4));
    struct TextContext{GuestCall* call;uint32_t writer,line_writer,line_rect,break_flag;} context{&call,writer,line_writer,line_rect,break_flag};
    const mh3_system_ui_text_callbacks io{&context,
        [](void*opaque,uint32_t cursor,uint32_t remaining,float wrap_width,float*line_width,uint8_t*line_break)->uint32_t{
            auto&c=*static_cast<TextContext*>(opaque);std::memcpy(Memory::GetPointer(c.line_writer,100),Memory::GetPointer(c.writer,100),100);
            Memory::WriteFloat32(c.line_writer+44,0);Memory::WriteFloat32(c.line_writer+48,0);for(unsigned i=0;i<4;++i)Memory::WriteFloat32(c.line_rect+i*4,0);
            const auto consumed=(*c.call)(0x8052dc20,{c.line_rect,c.line_writer,cursor,remaining,c.break_flag},{wrap_width});
            *line_width=Memory::ReadFloat32(c.line_rect+8)-Memory::ReadFloat32(c.line_rect);*line_break=Memory::Read8(c.break_flag);(*c.call)(0x805051cc,{c.line_writer,0xffffffff});return consumed;
        },
        [](void*opaque,uint32_t cursor,uint32_t consumed,float x){auto&c=*static_cast<TextContext*>(opaque);Memory::WriteFloat32(c.writer+44,x);(*c.call)(0x805052a0,{c.writer,cursor,consumed});},
        [](void*opaque){auto&c=*static_cast<TextContext*>(opaque);(*c.call)(0x805052a0,{c.writer,0x80796978,1});}};
    Checked(mh3_system_ui_text_lines(&io,text,Memory::Read16(self+0xfe),left,width,alignment,Memory::ReadFloat32(self+0x4c)));
    call(0x805051cc,{writer,0xffffffff});
}
REGISTER_NATIVE_FUNCTION_AS(0x8052D674,MH3_SystemUITextDraw,"SystemUITextDraw");
namespace {
void HudSetup(CpuContext*cpu){GuestCall call{cpu};call(0x8005af80);Ortho(0,Memory::ReadFloat32(0x8065b4c4),0,Memory::ReadFloat32(0x8065b4c0),0,1);Identity();
    auto m=GetNativeMaterialState();mh3_system_ui_hud_material(&m);
    Pixel(m,MH3_PIXEL_LOGIC_OPERATION,0);Pixel(m,MH3_PIXEL_COLOR_WRITE,1);Pixel(m,MH3_PIXEL_DEPTH_TEST,1);Pixel(m,MH3_PIXEL_DEPTH_COMPARE,7);Pixel(m,MH3_PIXEL_DEPTH_WRITE,0);
    SetNativeMaterial(m);format={true,1};layout_coordinates=false;
}
void HudBind(uint32_t object){auto binding=ReadNativeTextureObject(object);binding.map_id=0;BindNativeTexture(binding);}
void HudBlend(unsigned mode){auto r=GetNativeDrawState().raster;r.blend_mode=mode;r.src_factor=mode==3?2:4;r.dst_factor=mode==3?2:5;r.logic_op=0;SetNativeRaster(r);}
}
extern "C" void MH3_HUDTexgen(CpuContext*cpu){const unsigned slot=cpu->gpr[3];if(slot>=8||cpu->gpr[4]!=1||cpu->gpr[5]<4||cpu->gpr[5]>11||cpu->gpr[6]!=60)throw std::runtime_error("HUD texgen requires a verified direct UV input");auto m=GetNativeMaterialState();m.texgen[slot].known|=1u<<MH3_TEXGEN_PROJECTION;m.texgen[slot].values[MH3_TEXGEN_PROJECTION]=0;SetNativeMaterial(m);}
REGISTER_NATIVE_FUNCTION_AS(0x800D2130,MH3_HUDTexgen,"HUDTexgen");
extern "C" void MH3_HUDIcon(CpuContext*cpu){
    GuestFrame frame(cpu,0x10);GuestCall call{cpu};const uint8_t index=frame.Arg(3);HudSetup(cpu);HudBind(0x8069787c);const float width=Memory::Read16(0x806978a0),height=Memory::Read16(0x806978a2);if(!width||!height)throw std::runtime_error("HUD icon atlas has zero dimensions");
    const uint32_t tile=0x80595b10+Memory::Read8(0x8065b23c+index)*8;float uv[4];for(unsigned i=0;i<4;++i)uv[i]=int16_t(Memory::Read16(tile+i*2))/(i%2?height:width);
    const uint8_t white[4]={255,255,255,255};mh3_system_ui_vertex vertices[4];if(NativeGraphicsFrameShouldRender()){if(!mh3_system_ui_hud_rectangle(vertices,call(0x80040424)==1?770:577,index==1?95:47,32,32,white,uv[0],uv[1],uv[2],uv[3]))throw std::runtime_error("HUD icon position is unrepresentable");Submit(vertices,4);}
}
REGISTER_NATIVE_FUNCTION_AS(0x800D29DC,MH3_HUDIcon,"HUDIcon");
extern "C" void MH3_HUDDraw(CpuContext*cpu){
    GuestFrame frame(cpu,0x60);GuestCall call{cpu};if(!Memory::Read8(0x806977a0))return;
    if(Memory::Read8(0x80697846)==6&&!uint8_t(call(0x800d0744))){Memory::Write8(0x806977a0,0);return;}
    call(0x80040870);if(!Memory::Read8(0x8065aa65))call(0x800d09e4);HudSetup(cpu);
    float atlas_width,atlas_height;bool background=true;const uint8_t type=Memory::Read8(0x80697846);
    if(type==6){const auto shape=call(0x802e05b4,{0x11c6,0});
        if(shape){call(0x80054cd8,{Memory::Read8(shape+0x12),frame[0x10],frame[0x14]});auto binding=ReadNativeTextureObject(Memory::Read32(frame[0x10]));binding.map_id=0;
            const auto palette=Memory::Read32(frame[0x14]);if(palette)BindNativePalette(0,ReadNativePaletteObject(palette));BindNativeTexture(binding);atlas_width=atlas_height=256;background=false;
        }else{HudBind(0x806978ec);atlas_width=Memory::Read16(0x80697910);atlas_height=Memory::Read16(0x80697912);}}
    else {HudBind(0x8069787c);atlas_width=Memory::Read16(0x806978a0);atlas_height=Memory::Read16(0x806978a2);}
    if(background){
        if(NativeGraphicsFrameShouldRender()){int16_t origins[4][2];const uint32_t addresses[4]={0x80595ad4,0x80595ac8,0x80595acc,0x80595ad0};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<2;++j)origins[i][j]=Memory::Read16(addresses[i]+j*2);
            uint8_t color[4];for(unsigned i=0;i<4;++i)color[i]=Memory::Read8(0x80793828+i);mh3_system_ui_hud_quad quads[5];uint32_t count;Checked(mh3_system_ui_hud_background(type,call(0x80040424)==1,atlas_width,atlas_height,origins,color,quads,&count));
            for(unsigned i=0;i<count;++i){HudBlend(quads[i].blend_mode);Submit(quads[i].vertex,4);}HudBlend(1);
        }else HudBlend(1);
        if(type!=6){HudBind(0x806978b4);atlas_width=Memory::Read16(0x806978d8);atlas_height=Memory::Read16(0x806978da);}
    }
    if(!atlas_width||!atlas_height)throw std::runtime_error("HUD sprite atlas has zero dimensions");
    const uint32_t language=Memory::Read8(0x8065aa69),count=Memory::Read8(0x80793820+language),atlas=Memory::Read32(0x805959e8+language*4);
    if(NativeGraphicsFrameShouldRender())for(unsigned i=0;i<count;++i){
        const float x=float(int16_t(Memory::Read16(0x8069781a+i*2)))-(call(0x80040424)==0?160:0),y=int16_t(Memory::Read16(0x80697830+i*2));const auto angle=Memory::Read32(0x806977a8+i*4);
        const float u=float(int16_t(Memory::Read16(atlas+i*4)))/atlas_width,v=float(int16_t(Memory::Read16(atlas+i*4+2)))/atlas_height,ur=float(int16_t(Memory::Read16(atlas+i*4))+64)/atlas_width,vb=float(int16_t(Memory::Read16(atlas+i*4+2))+64)/atlas_height;
        mh3_system_ui_vertex vertices[4]{};
        for(unsigned j=0;j<4;++j){std::memcpy(Memory::GetPointer(frame[0x20],12),Memory::GetPointer(0x806977d4+j*12,12),12);if(angle)call(0x80051178,{frame[0x20],angle});
            vertices[j].position[0]=int16_t(int32_t(Memory::ReadFloat32(frame[0x20])+x));vertices[j].position[1]=int16_t(int32_t(Memory::ReadFloat32(frame[0x24])+y));
            vertices[j].coordinate[0][0]=j==1||j==2?ur:u;vertices[j].coordinate[0][1]=j>=2?vb:v;for(auto&coord:vertices[j].coordinate)coord[2]=1;std::fill(vertices[j].color,vertices[j].color+4,255);}
        Submit(vertices,4);
    }
    if(type!=6){if(Memory::Read8(0x8065b23c)!=255)call(0x800d29dc,{0});if(type>=4&&type<=5&&Memory::Read8(0x8065b23d)!=255)call(0x800d29dc,{1});}
    call(0x800885f4,{0x7ff});auto m=GetNativeMaterialState();m.indirect_count=0;SetNativeMaterial(m);
    if(type>=1&&type<=4){const float width=call(0x80040424)==1?854:640;uint32_t text=0x80697847;
        if(!Memory::Read8(text)){const auto strings=call(0x800d2e98,{0x3e});text=Memory::Read32(strings+(type==1||type==4?0:type==3?12:8));}
        size_t length=0;while(Memory::Read8(text+length))++length;call(0x8005b714,{22,22});call(0x8005b780,{uint32_t(int32_t((width-float(length*11))*.5f)),229});call(0x8005b7ec,{0xc0c0c0ff});call(0x8005ba04,{text});call(0x8005c174);
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x800D10B0,MH3_HUDDraw,"HUDDraw");
