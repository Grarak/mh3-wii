// ABI adapters for the typed NW4R effects C producers. No GX calls/FIFO writes.
#include "native_effect_draw.h"
#include "native_graphics.h"
#include "guest_call.h"
#include "memory.h"
#include "../native/mh3_effect_draw.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
using MemoryInline::FlatRead8;
using MemoryInline::FlatRead16;
using MemoryInline::FlatRead32;
using MemoryInline::FlatReadFloat32;
using MemoryInline::FlatWrite8;
using MemoryInline::FlatWrite32;
using MemoryInline::FlatWriteFloat32;

struct EffectInputs {
    mh3_effect_material resource{};
    float texture_matrix[8][12]{};
    bool position_source[8]{};
    uint32_t strategy=0;
    uint8_t line_size=0,point_size=0;
} effect;

mh3_res_span Span(uint32_t p,size_t n){return {Memory::GetPointer(p,n),n};}
void Require(mh3_res_status s,const char *what){if(s!=MH3_RES_OK)throw std::runtime_error(what);}
void ReadVector(uint32_t p,float *out,uint32_t count){for(uint32_t i=0;i<count;i++)out[i]=FlatReadFloat32(p+4*i);}
void WriteVector(uint32_t p,const float *v,uint32_t count){for(uint32_t i=0;i<count;i++)FlatWriteFloat32(p+4*i,v[i]);}
void CopyColor(uint32_t p,uint8_t out[4]){for(uint32_t i=0;i<4;i++)out[i]=FlatRead8(p+i);}
void StoreColor(uint32_t p,const uint8_t in[4]){for(uint32_t i=0;i<4;i++)FlatWrite8(p+i,in[i]);}
void LoadResource(uint32_t p){Require(mh3_effect_material_read(Span(p,0xac),&effect.resource),"Invalid NW4R effect draw resource");}

void TextureMaps(uint32_t strategy,uint32_t resource){
    LoadResource(resource);effect.strategy=strategy;
    FlatWrite8(strategy+0xd0,effect.resource.texture_count);
    for(uint32_t i=0;i<3;i++)FlatWrite32(strategy+0xd4+4*i,uint32_t(int32_t(effect.resource.texture_maps[i])));
    for(uint32_t i=0;i<8;i++){mh3_draw_identity_position(effect.texture_matrix[i]);effect.position_source[i]=false;}
}
void Material(uint32_t strategy,uint32_t resource,uint32_t info){
    LoadResource(resource);effect.strategy=strategy;
    mh3_native_material_state material;
    mh3_effect_material_apply(&effect.resource,&GetNativeMaterialState(),&material);
    SetNativeMaterial(material);
    mh3_raster_state raster=GetNativeDrawState().raster;
    raster.blend_mode=FlatRead8(resource+0x5c);raster.src_factor=FlatRead8(resource+0x5d);
    raster.dst_factor=FlatRead8(resource+0x5e);raster.logic_op=FlatRead8(resource+0x5f);
    raster.depth_test=(effect.resource.flags&1)!=0;raster.depth_write=(effect.resource.flags&2)!=0;
    raster.depth_compare=FlatRead8(resource+0x70);raster.cull_mode=0;SetNativeRaster(raster);
    SetNativeDepthCompareBeforeTexture((effect.resource.flags&4)!=0);
    mh3_native_fog fog{};fog.end=fog.far_plane=1;
    if(effect.resource.flags&0x1000){fog.type=FlatRead32(info+0x70);fog.start=FlatReadFloat32(info+0x74);fog.end=FlatReadFloat32(info+0x78);fog.near_plane=FlatReadFloat32(info+0x7c);fog.far_plane=FlatReadFloat32(info+0x80);CopyColor(info+0x84,reinterpret_cast<uint8_t*>(&fog.color));}
    SetNativeFog(fog);
}
void Channels(uint32_t strategy,uint32_t resource,uint32_t info){
    mh3_native_material_state material=GetNativeMaterialState();material.channel_count=1;
    auto &channel=material.channel[0];channel.flags=63;CopyColor(info+0x98,channel.material_color);CopyColor(info+0x9c,channel.ambient_color);
    const bool lighting=FlatRead8(info+0x60)!=0;
    channel.color={0,0,uint8_t(lighting&&FlatRead8(resource+0x60)==1),0,uint8_t(lighting&&FlatRead8(resource+0x60)==1?(FlatRead8(info+0x6c)?1:2):2),uint8_t(lighting&&FlatRead8(resource+0x60)==1?FlatRead32(info+0x64):0)};
    channel.alpha={0,0,uint8_t(lighting&&FlatRead8(resource+0x68)==1),0,uint8_t(lighting&&FlatRead8(resource+0x68)==1?(FlatRead8(info+0x6c)?1:2):2),uint8_t(lighting&&FlatRead8(resource+0x68)==1?FlatRead32(info+0x68):0)};
    material.channel[1].color=material.channel[1].alpha={0,0,0,0,2,0};material.channel[1].flags|=48;
    FlatWrite32(strategy+0xb0,0xffffffff);FlatWrite32(strategy+0xb4,0xffffffff);
    for(uint32_t i=0;i<8;i++)FlatWrite8(strategy+0xb8+i,0);
    for(uint32_t source=1;source<8;source++){
        const uint8_t selectors[2]={FlatRead8(resource+0x60+source),FlatRead8(resource+0x68+source)};
        for(uint32_t alpha=0;alpha<2;alpha++){
            const uint8_t s=selectors[alpha];const uint32_t base=strategy+0xb8+4*alpha;
            if(s>=1&&s<=4)FlatWrite8(base+s-1,1);
            if(s==5){FlatWrite8(base,1);FlatWrite8(base+1,1);}if(s==6){FlatWrite8(base+2,1);FlatWrite8(base+3,1);}
        }
    }
    SetNativeMaterial(material);
}
uint32_t Alpha(uint32_t strategy,uint32_t particle,uint32_t resource,uint32_t force){
    const uint8_t a=FlatRead8(particle+0x97);
    if(!force&&FlatRead32(strategy+0xb0)==a)return 0;
    FlatWrite32(strategy+0xb0,a);FlatWrite32(strategy+0xb4,a); // original caches reference0 in both slots
    mh3_native_material_state material=GetNativeMaterialState();auto &p=material.pixel;
    const uint32_t fields[5]={MH3_PIXEL_ALPHA_COMPARE0,MH3_PIXEL_ALPHA_REFERENCE0,MH3_PIXEL_ALPHA_OPERATION,MH3_PIXEL_ALPHA_COMPARE1,MH3_PIXEL_ALPHA_REFERENCE1};
    const uint32_t values[5]={FlatRead8(resource+2),a,FlatRead8(resource+4),FlatRead8(resource+3),FlatRead8(particle+0x98)};
    for(uint32_t i=0;i<5;i++){p.known|=UINT64_C(1)<<fields[i];p.values[fields[i]]=values[i];}SetNativeMaterial(material);return 1;
}
uint32_t Colors(CpuContext *cpu,uint32_t strategy,uint32_t particle,uint32_t resource,uint32_t force){
    GuestCall game{cpu};
    for(uint32_t i=0;i<4;i++)if(FlatRead8(strategy+0xb8+i))game.Call<0x800AB0BC>({particle,i/2,i%2,strategy+0xc0+4*i});
    uint32_t scale=255;bool alpha_used=false;for(uint32_t i=0;i<4;i++)alpha_used|=FlatRead8(strategy+0xbc+i)!=0;
    if(alpha_used&&FlatRead8(resource+0x71))scale=game.Call<0x800AB460>({particle})&255;
    for(uint32_t i=0;i<4;i++)if(FlatRead8(strategy+0xbc+i)){
        uint32_t a=game.Call<0x800AB284>({particle,i/2,i%2})&255;if(scale!=255)a=(a*scale+128)>>8;FlatWrite8(strategy+0xc3+4*i,uint8_t(a));
    }
    const uint32_t manager=FlatRead32(particle+0xc8);
    if(FlatRead8(manager+0x6c))for(uint32_t pair=0;pair<2;pair++){
        bool used=false;for(uint32_t i=2*pair;i<2*pair+2;i++)used|=FlatRead8(strategy+0xb8+i)||FlatRead8(strategy+0xbc+i);
        if(used)game.Call<0x800AE70C>({manager,particle,strategy+0xc0+8*pair,strategy+0xc4+8*pair});
    }
    mh3_effect_material m;Require(mh3_effect_material_read(Span(resource,0xac),&m),"Invalid live effect color selectors");
    uint8_t inputs[4][4],tev[3][4],konst[4][4];for(uint32_t i=0;i<4;i++)CopyColor(strategy+0xc0+4*i,inputs[i]);mh3_effect_register_colors(&m,inputs,tev,konst);
    mh3_native_material_state material=GetNativeMaterialState();uint32_t changed=0;
    for(uint32_t i=0;i<7;i++){
        if(!m.rgb_sources[i+1]&&!m.alpha_sources[i+1])continue;
        uint8_t *value=i<3?tev[i]:konst[i-3];uint32_t cache=strategy+(i<3?0x94+4*i:0xa0+4*(i-3));uint8_t previous[4];CopyColor(cache,previous);
        if(force||std::memcmp(previous,value,4)){
            StoreColor(cache,value);auto &reg=i<3?material.color[i+1]:material.konst[i-3];reg.known=15;for(uint32_t k=0;k<4;k++)reg.rgba[k]=value[k];changed=1;
        }
    }
    if(changed)SetNativeMaterial(material);return changed;
}
void ParticleState(CpuContext *cpu,uint32_t strategy,uint32_t particle,uint32_t resource,uint32_t info,uint32_t force){
    Alpha(strategy,particle,resource,force);Colors(cpu,strategy,particle,resource,force);
    GuestFrame args(cpu,0x30);cpu->gpr[3]=strategy;cpu->gpr[4]=particle;cpu->gpr[5]=resource;cpu->gpr[6]=info;cpu->gpr[7]=force;NativeEffectTextureSetup(cpu);
    // The stock routine emits eight degenerate vertices only to flush GX state.
    // Native submission snapshots the complete material; no pad geometry exists.
}
void Emit(const mh3_effect_quad &q,bool textured,const float (*coordinates)[2]=nullptr,uint32_t size_code=0,bool point=false,bool strip=false){
    AuroraNativeMaterialGeometry geometry{};if(!aurora_allocate_native_material_geometry(4,6,&geometry))return;
    const float defaults[4][2]={{0,1},{0,0},{1,0},{1,1}},zero[4][2]={};const auto &material=GetNativeMaterialState();mh3_effect_vertex_inputs input{};
    std::memcpy(input.texture_matrix,effect.texture_matrix,sizeof(input.texture_matrix));for(uint32_t i=0;i<8;i++)input.position_source[i]=effect.position_source[i];input.normal[0]=1;float normal_matrix[9];if(GetNativeNormalMatrix(0,normal_matrix)){for(uint32_t r=0;r<3;r++)input.normal[r]=normal_matrix[3*r];float n=std::sqrt(input.normal[0]*input.normal[0]+input.normal[1]*input.normal[1]+input.normal[2]*input.normal[2]);if(n>1e-5f)for(float &v:input.normal)v/=n;}else if(material.channel[0].color.lighting||material.channel[0].alpha.lighting){throw std::runtime_error("Lit effect has unresolved inherited normal matrix0");}for(uint32_t c=0;c<2;c++)std::memcpy(input.color[c],material.channel[c].material_color,4);
    mh3_effect_material_vertices(&GetNativeDrawState(),&q,coordinates?coordinates:textured?defaults:zero,&input,geometry.vertices);
    if(size_code){
        if(point)mh3_effect_expand_point(&GetNativeDrawState(),float(size_code)/6,geometry.vertices);else mh3_effect_expand_line(&GetNativeDrawState(),float(size_code)/6,geometry.vertices);
        for(uint32_t i=0;i<4;i++){if(point&&(i&1))geometry.vertices[i].texcoord[0][0]+=1;if(point&&i>=2)geometry.vertices[i].texcoord[0][1]+=1;if(!point&&(i&1))geometry.vertices[i].texcoord[0][1]+=1;}mh3_effect_strip_indices(4,geometry.indices);
    }else if(strip)mh3_effect_strip_indices(4,geometry.indices);else mh3_effect_quad_indices(0,geometry.indices);
    SubmitNativeMaterialGeometry(geometry);
}
struct BillboardContext {CpuContext *cpu;uint32_t strategy,info,manager,resource,first_fn,next_fn,rotation_out,sine_out;};
uint32_t First(void *opaque){auto &c=*static_cast<BillboardContext*>(opaque);return GuestCall{c.cpu}(c.first_fn,{c.manager});}
uint32_t Next(void *opaque,uint32_t p){auto &c=*static_cast<BillboardContext*>(opaque);return GuestCall{c.cpu}(c.next_fn,{c.manager,p});}
void ReadParticle(void *opaque,uint32_t p,mh3_effect_particle *out){
    auto &c=*static_cast<BillboardContext*>(opaque);*out={};ReadVector(p+0xac,out->position,3);ReadVector(p+0x30,out->size,2);ReadVector(p+0x38,out->scale,2);
    const uint32_t owner=FlatRead32(p+0xc8);ReadVector(owner+0x58,out->manager_scale,2);out->emitter_flags=FlatRead16(FlatRead32(owner+0x24)+0x9c);
}
void OrientParticle(void *opaque,uint32_t p,mh3_effect_particle *out){
    auto &c=*static_cast<BillboardContext*>(opaque);GuestCall game{c.cpu};game.Call<0x800B5518>({p,c.rotation_out});out->rotation=FlatReadFloat32(c.rotation_out+8);out->cosine=1;
    if(out->rotation!=0){game.Call<0x8009C7C4>({c.sine_out,c.sine_out+4},{-out->rotation});out->sine=FlatReadFloat32(c.sine_out);out->cosine=FlatReadFloat32(c.sine_out+4);}
}
void Prepare(void *opaque,uint32_t p,int first){auto &c=*static_cast<BillboardContext*>(opaque);ParticleState(c.cpu,c.strategy,p,c.resource,c.info,uint32_t(first));}
void SubmitBillboard(void*,const mh3_effect_quad *q){Emit(*q,effect.resource.texture_count!=0);}
}

uint32_t NativeEffectTextureSetup(CpuContext *cpu){
    GuestFrame frame(cpu,0x40);const uint32_t strategy=frame.Arg(3),particle=frame.Arg(4),flags=frame.Arg(5),info=frame.Arg(6),force=frame.Arg(7);GuestCall game{cpu};uint32_t changed=0;
    for(uint32_t slot=0;slot<3;slot++){
        int32_t map=int32_t(FlatRead32(strategy+0xd4+4*slot));if(map<0)continue;if(map>=8)throw std::runtime_error("Effect texture map exceeds native material layout");
        uint32_t texture=FlatRead32(particle+0x88+4*slot);if(!texture)continue;uint32_t cache=strategy+0x30*slot,params=particle+8*slot,wrap=FlatRead16(particle+0x94),ws=(wrap>>(4*slot))&3,wt=(wrap>>(4*slot+2))&3;
        if(force||FlatRead32(cache+4)!=texture||FlatRead32(cache+0x18)!=ws||FlatRead32(cache+0x1c)!=wt){
            mh3_native_texture_binding binding;mh3_native_palette_source palette;Require(mh3_effect_texture_read(Span(texture,0x24),texture,uint32_t(map),ws,wt,&binding,&palette),"Invalid effect texture source");
            if(binding.source.indexed)BindNativePalette(uint32_t(map),palette);BindNativeTexture(binding);FlatWrite32(cache+4,texture);FlatWrite32(cache+0x18,ws);FlatWrite32(cache+0x1c,wt);changed=1;
        }
        uint32_t mirror=(FlatRead8(particle+0x96)>>(2*slot))&3;mh3_effect_texture_transform t{};
        for(uint32_t axis=0;axis<2;axis++){uint32_t w=axis?wt:ws;bool reflected=(mirror&(1u<<axis))!=0;t.scale[axis]=float(w==2?2:1)*(reflected?-1:1);t.translation[axis]=float(reflected?(w==2?2:1):0);t.tile[axis]=FlatReadFloat32(params+0x4c+4*axis);t.offset[axis]=FlatReadFloat32(params+0x70+4*axis);}
        t.rotation=FlatReadFloat32(particle+0x64+4*slot);t.position_source=(FlatRead16(flags)>>(slot+7))&1;
        float inputs[9]={t.scale[0],t.scale[1],t.translation[0],t.translation[1],t.tile[0],t.tile[1],t.rotation,t.offset[0],t.offset[1]};const uint32_t offsets[9]={8,12,16,20,32,36,40,44,48};bool update=force!=0;for(uint32_t i=0;i<9;i++)update|=inputs[i]!=FlatReadFloat32(cache+offsets[i]);if(!update)continue;
        for(uint32_t i=0;i<9;i++)FlatWriteFloat32(cache+offsets[i],inputs[i]);changed=1;
        const uint32_t resource=FlatRead32(FlatRead32(particle+0xc8)+0x24)+0x94;bool opposite=(FlatRead8(particle+0xe5)&1)!=0;uint8_t high=FlatRead8(resource+2),low=FlatRead8(resource+1);
        if(slot==0){if((high&4)&&opposite)t.flip_s=t.flip_t=1;else{t.flip_s=(high&8)!=0;t.flip_t=(high&16)!=0;}}
        if(slot==1){if((high&32)&&opposite)t.flip_s=t.flip_t=1;else{t.flip_s=(low&64)!=0;t.flip_t=(low&128)!=0;}}
        float sine=0,cosine=1;if(t.rotation!=0){game.Call<0x8009C7C4>({frame[0x10],frame[0x14]},{t.rotation});sine=FlatReadFloat32(frame[0x10]);cosine=FlatReadFloat32(frame[0x14]);}
        float post[12];ReadVector(info+0x30,post,12);mh3_effect_texture_matrix(&t,sine,cosine,post,effect.texture_matrix[map]);effect.position_source[map]=t.position_source;
        SetNativeTextureMatrix(t.position_source?3*uint32_t(map)+0x40:3*uint32_t(map)+0x1e,effect.texture_matrix[map],t.position_source);
        if(force){mh3_native_material_state material=GetNativeMaterialState();auto &gen=material.texgen[map];gen.known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;std::memset(gen.values,0,sizeof(gen.values));gen.values[MH3_TEXGEN_PROJECTION]=t.position_source?1:0;gen.values[MH3_TEXGEN_SOURCE_ROW]=t.position_source?0:5;gen.values[MH3_TEXGEN_POST_MATRIX_ROW]=t.position_source?uint8_t(3*map):61;SetNativeMaterial(material);}
    }
    return changed;
}

void NativeEffectDrawBillboards(CpuContext *cpu){
    GuestFrame frame(cpu,0xa0);BillboardContext c{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(5),0,0,0,frame[0x70],frame[0x80]};GuestCall game{cpu};c.resource=FlatRead32(c.manager+0x24)+0x9c;
    TextureMaps(c.strategy,c.resource);Material(c.strategy,c.resource,c.info);Channels(c.strategy,c.resource,c.info);
    float identity[12];mh3_draw_identity_position(identity);SetNativePositionMatrix(identity);
    game.Call<0x800B7E14>({c.info,frame[0x10]});game.Call<0x800AE3C4>({c.manager,frame[0x40]});
    float view[12],local[12],matrix[12];ReadVector(frame[0x10],view,12);ReadVector(frame[0x40],local,12);
    for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)matrix[4*r+col]=view[4*r]*local[col]+view[4*r+1]*local[4+col]+view[4*r+2]*local[8+col]+(col==3?view[4*r+3]:0);
    float pivot[2]={float(int8_t(FlatRead8(c.resource+0xa9)))/100,float(int8_t(FlatRead8(c.resource+0xaa)))/100};mh3_effect_billboard draw;mh3_effect_billboard_axes(&draw,matrix,view,pivot,FlatRead8(c.resource+0xad));
    uint32_t order=FlatRead16(c.resource)&0x800;c.first_fn=game.Virtual<0x10>(c.strategy,{order});c.next_fn=game.Virtual<0x14>(c.strategy,{order});
    mh3_effect_billboard_source source{&c,First,Next,ReadParticle,Prepare,OrientParticle,SubmitBillboard};mh3_effect_draw_billboards(&draw,FlatReadFloat32(0x80798600),&source);
}

extern "C" void MH3_EffectTextureMaps(CpuContext *c){TextureMaps(c->gpr[3],c->gpr[4]);}
extern "C" void MH3_EffectMaterial(CpuContext *c){Material(c->gpr[3],c->gpr[4],c->gpr[5]);}
extern "C" void MH3_EffectChannels(CpuContext *c){Channels(c->gpr[3],c->gpr[5],c->gpr[6]);}
extern "C" void MH3_EffectSetup(CpuContext *c){uint32_t strategy=c->gpr[3],resource=c->gpr[5],info=c->gpr[6];TextureMaps(strategy,resource);Material(strategy,resource,info);Channels(strategy,resource,info);}
extern "C" void MH3_EffectAlpha(CpuContext *c){c->gpr[3]=Alpha(c->gpr[3],c->gpr[4],c->gpr[5],c->gpr[6]);}
extern "C" void MH3_EffectColors(CpuContext *c){uint32_t result;{GuestFrame f(c,0x30);result=Colors(c,f.Arg(3),f.Arg(4),f.Arg(5),f.Arg(6));}c->gpr[3]=result;}
extern "C" void MH3_EffectParticleState(CpuContext *c){GuestFrame f(c,0x30);ParticleState(c,f.Arg(3),f.Arg(4),f.Arg(5),f.Arg(6),f.Arg(7));}
extern "C" void MH3_EffectBillboardSetup(CpuContext *c){uint32_t strategy=c->gpr[3],info=c->gpr[4],resource=FlatRead32(c->gpr[5]+0x24)+0x9c;TextureMaps(strategy,resource);Material(strategy,resource,info);Channels(strategy,resource,info);float identity[12];mh3_draw_identity_position(identity);SetNativePositionMatrix(identity);}
extern "C" void MH3_EffectAxesQuad(CpuContext *c){float center[3],a[3],b[3];ReadVector(c->gpr[4],center,3);ReadVector(c->gpr[5],a,3);ReadVector(c->gpr[6],b,3);mh3_effect_quad q;mh3_effect_quad_axes(center,a,b,&q);Emit(q,c->gpr[7]!=0);}
extern "C" void MH3_EffectDirectionalAxesQuad(CpuContext *c){float center[3],a[3],b[3];ReadVector(c->gpr[3],center,3);ReadVector(c->gpr[4],a,3);ReadVector(c->gpr[5],b,3);mh3_effect_quad q;mh3_effect_quad_axes(center,a,b,&q);Emit(q,c->gpr[6]!=0);}
extern "C" void MH3_EffectTransformedQuad(CpuContext *c){float matrix[12];mh3_effect_quad q,out;ReadVector(c->gpr[3],matrix,12);for(uint32_t i=0;i<4;i++)ReadVector(c->gpr[4]+12*i,q.position[i],3);mh3_effect_quad_transform(matrix,&q,&out);Emit(out,c->gpr[5]!=0);}
extern "C" void MH3_EffectViewMatrix(CpuContext *c){uint32_t info=c->gpr[3],output=c->gpr[4];float view[12];ReadVector(info,view,12);float bias=FlatReadFloat32(info+0x88);if(bias!=0){if(GetNativeDrawState().perspective){float anchor[3],v[3];ReadVector(info+0x8c,anchor,3);for(uint32_t r=0;r<3;r++)v[r]=view[4*r]*anchor[0]+view[4*r+1]*anchor[1]+view[4*r+2]*anchor[2]+view[4*r+3];float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(length==0)view[11]+=bias;else for(uint32_t r=0;r<3;r++)view[4*r+3]+=v[r]*(v[2]<0?-bias:bias)/length;}else view[11]+=bias;}WriteVector(output,view,12);c->gpr[3]=output;}
REGISTER_NATIVE_FUNCTION_AS(0x800C6140,MH3_EffectTextureMaps,"EffectTextureMaps");
REGISTER_NATIVE_FUNCTION_AS(0x800C61BC,MH3_EffectMaterial,"EffectMaterial");
REGISTER_NATIVE_FUNCTION_AS(0x800C6548,MH3_EffectChannels,"EffectChannels");
REGISTER_NATIVE_FUNCTION_AS(0x800C60C8,MH3_EffectSetup,"EffectSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800C72D4,MH3_EffectAlpha,"EffectAlpha");
REGISTER_NATIVE_FUNCTION_AS(0x800C745C,MH3_EffectColors,"EffectColors");
REGISTER_NATIVE_FUNCTION_AS(0x800C694C,MH3_EffectParticleState,"EffectParticleState");
REGISTER_NATIVE_FUNCTION_AS(0x800BB7AC,MH3_EffectBillboardSetup,"EffectBillboardSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800BA798,MH3_EffectAxesQuad,"EffectBillboardAxesQuad");
REGISTER_NATIVE_FUNCTION_AS(0x800BC0F8,MH3_EffectDirectionalAxesQuad,"EffectDirectionalAxesQuad");
REGISTER_NATIVE_FUNCTION_AS(0x800BBE8C,MH3_EffectTransformedQuad,"EffectDirectionalQuad");
REGISTER_NATIVE_FUNCTION_AS(0x800BE1B8,MH3_EffectTransformedQuad,"EffectFreeQuad");
REGISTER_NATIVE_FUNCTION_AS(0x800B7E14,MH3_EffectViewMatrix,"EffectViewMatrix");

namespace {uint64_t movie_generation=1;}
void NativeMovieFrameDecoded(){++movie_generation;}
extern "C" void MH3_NativeMovieSetup(CpuContext *c){
    uint32_t mode=c->gpr[3];float width=float(FlatRead16(mode+4)),height=float(FlatRead16(mode+6)),projection[16],identity[12];
    if(!mh3_draw_ortho(projection,0,height,0,width,0,-1))throw std::runtime_error("Invalid movie viewport");
    mh3_draw_identity_position(identity);SetNativeProjection(projection,false);SetNativePositionMatrix(identity);SetNativeViewport(0,0,width,height,0,1);SetNativeScissor(0,0,int32_t(width),int32_t(height));
    mh3_native_material_state material;mh3_effect_movie_material(&GetNativeMaterialState(),&material);SetNativeMaterial(material);
    auto raster=GetNativeDrawState().raster;raster.depth_test=1;raster.depth_compare=7;raster.depth_write=0;raster.blend_mode=0;raster.src_factor=1;raster.dst_factor=0;raster.logic_op=0;raster.color_write=1;raster.alpha_write=0;SetNativeRaster(raster);
}
extern "C" void MH3_NativeMovieRestore(CpuContext*){
    mh3_native_material_state material;mh3_effect_movie_restore(&GetNativeMaterialState(),&material);SetNativeMaterial(material);
    auto raster=GetNativeDrawState().raster;raster.depth_test=1;raster.depth_compare=7;raster.depth_write=0;raster.blend_mode=0;raster.src_factor=1;raster.dst_factor=0;raster.logic_op=15;SetNativeRaster(raster);
}
extern "C" void MH3_NativeMoviePlanes(CpuContext *c){
    const uint32_t addresses[3]={c->gpr[3],c->gpr[4],c->gpr[5]};uint32_t width=uint16_t(c->gpr[8]),height=uint16_t(c->gpr[9]);
    for(uint32_t i=0;i<3;i++){
        mh3_native_texture_binding binding{};binding.source.payload_address=binding.source.resource_address=addresses[i];binding.source.format=1;binding.source.width=i?width/2:width;binding.source.height=i?height/2:height;binding.source.levels=1;binding.source.mutable_source=1;binding.source.generation=movie_generation;binding.map_id=i;BindNativeTexture(binding);
    }
    mh3_effect_quad q;int16_t draw_width=int16_t(c->gpr[10]),draw_height=int16_t(FlatRead32(c->gpr[1]+8));mh3_effect_movie_quad(int16_t(c->gpr[6]),int16_t(c->gpr[7]),draw_width,draw_height,int16_t(FlatRead32(0x806d6b9c)),&q);
    AuroraNativeMaterialGeometry geometry{};if(!aurora_allocate_native_material_geometry(4,6,&geometry))return;
    const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};const uint8_t white[4]={255,255,255,255};
    for(uint32_t i=0;i<4;i++){
        auto &v=geometry.vertices[i];std::memset(&v,0,sizeof(v));AuroraNativeVertex simple{};mh3_draw_vertex(&GetNativeDrawState(),q.position[i],uv[i],white,&simple);std::memcpy(v.position,simple.position,16);std::memcpy(v.view_position,q.position[i],12);v.normal[0]=1;std::memset(v.color,255,sizeof(v.color));
        for(uint32_t map=0;map<8;map++){v.texcoord[map][0]=uv[i][0];v.texcoord[map][1]=uv[i][1];v.texcoord[map][2]=1;}
    }
    mh3_effect_quad_indices(0,geometry.indices);SubmitNativeMaterialGeometry(geometry);
}
extern "C" void MH3_NativeMovieTexture(CpuContext *c){
    uint32_t mode=c->gpr[3],x=c->gpr[4],y=c->gpr[5],w=c->gpr[6],h=c->gpr[7];int32_t result=int32_t(FlatRead32(0x806d6cd8));
    if(result<0){c->gpr[3]=0xffffffff;return;}
    {GuestFrame frame(c,0x30);c->gpr[3]=mode;MH3_NativeMovieSetup(c);c->gpr[3]=FlatRead32(0x806d6ccc);c->gpr[4]=FlatRead32(0x806d6cd0);c->gpr[5]=FlatRead32(0x806d6cd4);c->gpr[6]=x;c->gpr[7]=y;c->gpr[8]=FlatRead32(0x806d6c20);c->gpr[9]=FlatRead32(0x806d6c24);c->gpr[10]=w;FlatWrite32(frame[8],h);MH3_NativeMoviePlanes(c);MH3_NativeMovieRestore(c);}c->gpr[3]=uint32_t(result);
}
extern "C" void MH3_EffectTexCoordGenerator(CpuContext *c){
    /*800C86D8 is the four-argument stub, like the movie/UI wrappers. */
    c->gpr[7]=0;c->gpr[8]=125;
    uint32_t index=c->gpr[3];if(index>=8)throw std::runtime_error("Effect texgen index exceeds material layout");
    auto material=GetNativeMaterialState();auto &gen=material.texgen[index];gen.known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;std::memset(gen.values,0,sizeof(gen.values));gen.values[MH3_TEXGEN_PROJECTION]=c->gpr[4]==0;gen.values[MH3_TEXGEN_SOURCE_ROW]=c->gpr[5]==4?5:uint8_t(c->gpr[5]);gen.values[MH3_TEXGEN_MATRIX_ROW]=uint8_t(c->gpr[6]);gen.values[MH3_TEXGEN_NORMALIZE]=c->gpr[7]!=0;gen.values[MH3_TEXGEN_POST_MATRIX_ROW]=uint8_t(c->gpr[8]-0x40);SetNativeMaterial(material);
}
extern "C" void MH3_NativeMovieTexgen(CpuContext *c){c->gpr[7]=0;c->gpr[8]=125;MH3_EffectTexCoordGenerator(c);}
REGISTER_NATIVE_FUNCTION_AS(0x804351C4,MH3_NativeMovieTexture,"NativeMovieTexture");
REGISTER_NATIVE_FUNCTION_AS(0x804352C8,MH3_NativeMovieRestore,"NativeMovieRestore");
REGISTER_NATIVE_FUNCTION_AS(0x804353E0,MH3_NativeMovieSetup,"NativeMovieSetup");
REGISTER_NATIVE_FUNCTION_AS(0x80435894,MH3_NativeMoviePlanes,"NativeMoviePlanes");
REGISTER_NATIVE_FUNCTION_AS(0x80435888,MH3_NativeMovieTexgen,"NativeMovieTexgen");
REGISTER_NATIVE_FUNCTION_AS(0x800C86D8,MH3_EffectTexCoordGenerator,"EffectTexgen");

namespace {
struct FreeContext {BillboardContext base;uint32_t rotation_matrix,draw_matrix;float pivot[2];uint8_t rotation_kind,cross;};
void FreeQuad(void *opaque,uint32_t id,const mh3_effect_particle *values,float width,float height){
    auto &f=*static_cast<FreeContext*>(opaque);auto &c=f.base;GuestCall game{c.cpu};
    game.Call<0x800BEA64>({f.rotation_matrix,id,f.rotation_kind});game.Call<0x800BE99C>({f.draw_matrix,f.rotation_matrix},{f.pivot[0],f.pivot[1],width,height});
    float matrix[12];ReadVector(f.draw_matrix,matrix,12);for(uint32_t r=0;r<3;r++)matrix[4*r+3]+=values->position[r];
    for(uint32_t plane=0;plane<1u+f.cross;plane++){mh3_effect_quad input,output;for(uint32_t v=0;v<4;v++)ReadVector(0x8056ff90+0x30*plane+12*v,input.position[v],3);mh3_effect_quad_transform(matrix,&input,&output);Emit(output,effect.resource.texture_count!=0);}
}
}
extern "C" void MH3_EffectFreeStrategy(CpuContext *cpu){
    GuestFrame frame(cpu,0x100);FreeContext f{{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(5),0,0,0,0,0},frame[0x70],frame[0xa0],{},0,0};auto &c=f.base;c.resource=FlatRead32(c.manager+0x24)+0x9c;GuestCall game{cpu};
    TextureMaps(c.strategy,c.resource);Material(c.strategy,c.resource,c.info);Channels(c.strategy,c.resource,c.info);
    game.Call<0x800B7E14>({c.info,frame[0x10]});game.Call<0x800AE3C4>({c.manager,frame[0x40]});float view[12],local[12],matrix[12];ReadVector(frame[0x10],view,12);ReadVector(frame[0x40],local,12);
    for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)matrix[4*r+col]=view[4*r]*local[col]+view[4*r+1]*local[4+col]+view[4*r+2]*local[8+col]+(col==3?view[4*r+3]:0);SetNativePositionMatrix(matrix);
    f.pivot[0]=float(int8_t(FlatRead8(c.resource+0xa9)))/100;f.pivot[1]=float(int8_t(FlatRead8(c.resource+0xaa)))/100;f.rotation_kind=FlatRead8(c.resource+0xaf);f.cross=FlatRead8(c.resource+0xad)==1;
    uint32_t order=FlatRead16(c.resource)&0x800;c.first_fn=game.Virtual<0x10>(c.strategy,{order});c.next_fn=game.Virtual<0x14>(c.strategy,{order});mh3_effect_billboard_source source{&f,First,Next,ReadParticle,Prepare,nullptr,nullptr};mh3_effect_draw_particles(FlatReadFloat32(0x80798654),&source,FreeQuad);
}
REGISTER_NATIVE_FUNCTION_AS(0x800BE424,MH3_EffectFreeStrategy,"EffectFreeStrategy");

namespace {
struct SmallParticleContext {BillboardContext base;uint32_t movement_out,last_size=0;bool point=false;};
void PrepareSmall(void*,uint32_t,int){}
void DrawSmall(void *opaque,uint32_t id,const mh3_effect_particle *p,float width,float height){
    auto &s=*static_cast<SmallParticleContext*>(opaque);auto &c=s.base;uint32_t limit=s.point?0x80793810:0x80793808;float scale=FlatReadFloat32(s.point?0x80798678:0x80798668);uint32_t code=uint32_t(double(scale)*double(width<FlatReadFloat32(limit)?width:FlatReadFloat32(limit)))&255;if(!code)return;
    mh3_effect_quad q{};const float point_uv[4][2]={},line_uv[4][2]={{0,0},{0,0},{0,1},{0,1}};
    if(s.point)for(uint32_t i=0;i<4;i++)std::memcpy(q.position[i],p->position,12);
    else{GuestCall game{c.cpu};game.Call<0x800A7F64>({id,s.movement_out});float movement[3];ReadVector(s.movement_out,movement,3);float length=std::sqrt(movement[0]*movement[0]+movement[1]*movement[1]+movement[2]*movement[2]);if(length==0)return;for(uint32_t i=0;i<4;i++)for(uint32_t k=0;k<3;k++)q.position[i][k]=i<2?p->position[k]:p->position[k]-movement[k]/length*height;}
    bool first=s.last_size==0;ParticleState(c.cpu,c.strategy,id,c.resource,c.info,uint32_t(first));s.last_size=code;if(s.point)effect.point_size=uint8_t(code);else effect.line_size=uint8_t(code);Emit(q,effect.resource.texture_count!=0,s.point?point_uv:line_uv,code,s.point);
}
void SmallStrategy(CpuContext *cpu,bool point){
    GuestFrame frame(cpu,0xc0);SmallParticleContext s{{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(5),0,0,0,0,0},frame[0x70],0,point};auto &c=s.base;c.resource=FlatRead32(c.manager+0x24)+0x9c;GuestCall game{cpu};
    TextureMaps(c.strategy,c.resource);Material(c.strategy,c.resource,c.info);Channels(c.strategy,c.resource,c.info);game.Call<0x800B7E14>({c.info,frame[0x10]});game.Call<0x800AE3C4>({c.manager,frame[0x40]});float view[12],local[12],matrix[12];ReadVector(frame[0x10],view,12);ReadVector(frame[0x40],local,12);
    for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)matrix[4*r+col]=view[4*r]*local[col]+view[4*r+1]*local[4+col]+view[4*r+2]*local[8+col]+(col==3?view[4*r+3]:0);SetNativePositionMatrix(matrix);
    uint32_t order=FlatRead16(c.resource)&0x800;c.first_fn=game.Virtual<0x10>(c.strategy,{order});c.next_fn=game.Virtual<0x14>(c.strategy,{order});mh3_effect_billboard_source source{&s,First,Next,ReadParticle,PrepareSmall,nullptr,nullptr};source.width_only=point;mh3_effect_draw_particles(FlatReadFloat32(0x80798600),&source,DrawSmall);
}
}
extern "C" void MH3_EffectLineStrategy(CpuContext *c){SmallStrategy(c,false);}
extern "C" void MH3_EffectPointStrategy(CpuContext *c){SmallStrategy(c,true);}
REGISTER_NATIVE_FUNCTION_AS(0x800BEFFC,MH3_EffectLineStrategy,"EffectLineStrategy");
REGISTER_NATIVE_FUNCTION_AS(0x800BF87C,MH3_EffectPointStrategy,"EffectPointStrategy");
extern "C" void MH3_EffectBillboardQuad(CpuContext *cpu){
    const float sx=float(cpu->fpr[1].d),sy=float(cpu->fpr[2].d),cosine=float(cpu->fpr[4].d),sine=float(cpu->fpr[5].d),width=float(cpu->fpr[6].d),height=float(cpu->fpr[7].d);
    GuestFrame frame(cpu,0x40);uint32_t particle=frame.Arg(4);mh3_effect_billboard draw{};ReadVector(frame.Arg(5),draw.matrix,12);ReadVector(frame.Arg(6),draw.pivot,2);draw.axis_x[0]=sx*cosine;draw.axis_x[1]=sx*sine;draw.axis_y[0]=sy*sine;draw.axis_y[1]=-sy*cosine;float position[3];ReadVector(particle+0xac,position,3);GuestCall game{cpu};game.Call<0x800B5518>({particle,frame[0x10]});float angle=FlatReadFloat32(frame[0x18]),sn=0,cs=1;if(angle!=0){game.Call<0x8009C7C4>({frame[0x20],frame[0x24]},{-angle});sn=FlatReadFloat32(frame[0x20]);cs=FlatReadFloat32(frame[0x24]);}mh3_effect_quad q;mh3_effect_billboard_quad(&draw,position,width,height,angle,sn,cs,&q);Emit(q,frame.Arg(7)!=0);
}
extern "C" void MH3_EffectStrategySetup(CpuContext *cpu){uint32_t strategy=cpu->gpr[3],info=cpu->gpr[4],resource=FlatRead32(cpu->gpr[5]+0x24)+0x9c;TextureMaps(strategy,resource);Material(strategy,resource,info);Channels(strategy,resource,info);}
extern "C" void MH3_EffectCullPadding(CpuContext *cpu){auto raster=GetNativeDrawState().raster;raster.cull_mode=cpu->gpr[3];SetNativeRaster(raster);}
REGISTER_NATIVE_FUNCTION_AS(0x800BA244,MH3_EffectBillboardQuad,"EffectBillboardQuad");
REGISTER_NATIVE_FUNCTION_AS(0x800BDBC4,MH3_EffectStrategySetup,"EffectDirectionalSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800BED90,MH3_EffectStrategySetup,"EffectFreeSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800BF62C,MH3_EffectStrategySetup,"EffectLineSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800BFDE8,MH3_EffectStrategySetup,"EffectPointSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800B8030,MH3_EffectStrategySetup,"EffectStripeSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800C4594,MH3_EffectStrategySetup,"EffectSmoothStripeSetup");
REGISTER_NATIVE_FUNCTION_AS(0x800B768C,MH3_EffectCullPadding,"EffectStripeCullPadding");
REGISTER_NATIVE_FUNCTION_AS(0x800C3D5C,MH3_EffectCullPadding,"EffectSmoothStripeCullPadding");
namespace {
struct DirectionalContext {BillboardContext base;uint32_t context,direction_fn,rotation_matrix,size_matrix,movement_out,direction_out;mh3_effect_directional_inputs inputs;float pivot[2];};
void DirectionalQuad(void *opaque,uint32_t id,const mh3_effect_particle *p,float width,float height){
    auto &d=*static_cast<DirectionalContext*>(opaque);auto &c=d.base;GuestCall game{c.cpu};float stretch=1;
    if(FlatRead8(c.resource+0xb0)){game.Call<0x800A7F64>({id,d.movement_out});float movement[3];ReadVector(d.movement_out,movement,3);float magnitude=std::sqrt(movement[0]*movement[0]+movement[1]*movement[1]+movement[2]*movement[2]);stretch=1+float(float(FlatReadFloat32(0x8079863c)*magnitude)/height);}
    game.Call<0x800BCEFC>({d.rotation_matrix,id,FlatRead8(c.resource+0xaf)});game.Call<0x800BCD78>({d.size_matrix,FlatRead8(c.resource+0xb1),d.rotation_matrix},{d.pivot[0],d.pivot[1],width,height,stretch});ReadVector(d.size_matrix,d.inputs.size_matrix,12);
    game(d.direction_fn,{d.direction_out,d.context,id});ReadVector(d.direction_out,d.inputs.direction,3);ReadVector(id+0xcc,d.inputs.history,3);std::memcpy(d.inputs.position,p->position,12);float history[3],matrix[12];mh3_effect_directional_matrix(&d.inputs,history,matrix);WriteVector(id+0xcc,history,3);
    uint32_t planes=1u+(FlatRead8(c.resource+0xad)==1);
    for(uint32_t plane=0;plane<planes;plane++){
        mh3_effect_quad q;
        if(d.inputs.screen_facing){float center[3],a[3],b[3];for(uint32_t r=0;r<3;r++){center[r]=matrix[4*r+3];a[r]=matrix[4*r+plane]+matrix[4*r+plane+1];b[r]=matrix[4*r+plane]-matrix[4*r+plane+1];}mh3_effect_quad_axes(center,a,b,&q);}
        else{mh3_effect_quad canonical;for(uint32_t v=0;v<4;v++)ReadVector(0x8056ff30+0x30*plane+12*v,canonical.position[v],3);mh3_effect_quad_transform(matrix,&canonical,&q);}
        Emit(q,effect.resource.texture_count!=0);
    }
}
void DirectionalStrategy(CpuContext *cpu,bool screen){
    GuestFrame frame(cpu,0x240);DirectionalContext d{{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(5),0,0,0,0,0},frame[0x70],0,frame[0x140],frame[0x180],frame[0x1c0],frame[0x1d0],{}, {}};auto &c=d.base;c.resource=FlatRead32(c.manager+0x24)+0x9c;GuestCall game{cpu};
    TextureMaps(c.strategy,c.resource);Material(c.strategy,c.resource,c.info);Channels(c.strategy,c.resource,c.info);game.Call<0x800B7E14>({c.info,frame[0x10]});game.Call<0x800C8F74>({d.context,frame[0x10],c.manager});
    float view[12],local[12];ReadVector(frame[0x10],view,12);ReadVector(d.context+0x38,local,12);for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)d.inputs.view_local[4*r+col]=view[4*r]*local[col]+view[4*r+1]*local[4+col]+view[4*r+2]*local[8+col]+(col==3?view[4*r+3]:0);d.inputs.screen_facing=screen;
    float initial[3];
    if(screen){ReadVector(0x806970ac,initial,3);ReadVector(0x80697088,d.inputs.fallback,3);float identity[12];ReadVector(0x806970b8,identity,12);SetNativePositionMatrix(identity);}
    else{SetNativePositionMatrix(d.inputs.view_local);float inverse[12],global[12],basis[12];ReadVector(d.context+0x68,inverse,12);if(FlatRead8(c.resource+0xae)==7)std::memcpy(basis,inverse,48);else{ReadVector(d.context+8,global,12);for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)basis[4*r+col]=inverse[4*r]*global[col]+inverse[4*r+1]*global[4+col]+inverse[4*r+2]*global[8+col]+(col==3?inverse[4*r+3]:0);}for(uint32_t r=0;r<3;r++){initial[r]=basis[4*r+1];d.inputs.fallback[r]=basis[4*r+2];}float n=std::sqrt(d.inputs.fallback[0]*d.inputs.fallback[0]+d.inputs.fallback[1]*d.inputs.fallback[1]+d.inputs.fallback[2]*d.inputs.fallback[2]);if(n!=0)for(float &v:d.inputs.fallback)v/=n;}
    for(uint32_t p=game.Call<0x800B5BA4>({c.manager});p&&FlatReadFloat32(p+0xcc)>1;p=game.Call<0x800B5B98>({c.manager,p}))WriteVector(p+0xcc,initial,3);
    d.direction_fn=game.Virtual<0x18>(c.strategy,{c.manager});uint32_t order=FlatRead16(c.resource)&0x800;c.first_fn=game.Virtual<0x10>(c.strategy,{order});c.next_fn=game.Virtual<0x14>(c.strategy,{order});d.pivot[0]=float(int8_t(FlatRead8(c.resource+0xa9)))/100;d.pivot[1]=float(int8_t(FlatRead8(c.resource+0xaa)))/100;
    mh3_effect_billboard_source source{&d,First,Next,ReadParticle,Prepare,nullptr,nullptr};mh3_effect_draw_particles(FlatReadFloat32(0x80798638),&source,DirectionalQuad);
}
}
extern "C" void MH3_EffectDirectionalStrategy(CpuContext *cpu){DirectionalStrategy(cpu,false);}
extern "C" void MH3_EffectDirectionalBillboardStrategy(CpuContext *cpu){DirectionalStrategy(cpu,true);}
REGISTER_NATIVE_FUNCTION_AS(0x800BC48C,MH3_EffectDirectionalStrategy,"EffectDirectionalStrategy");
REGISTER_NATIVE_FUNCTION_AS(0x800BD298,MH3_EffectDirectionalBillboardStrategy,"EffectDirectionalBillboardStrategy");
namespace {
struct RibbonContext {CpuContext *cpu;uint32_t strategy,context,flags,direction_fn,first_fn,next_fn,manager,orientation,size_matrix,direction_out;float pivot;float axes[2][3];};
uint32_t RibbonFirst(void *opaque){auto &r=*static_cast<RibbonContext*>(opaque);return GuestCall{r.cpu}(r.first_fn,{r.manager});}
uint32_t RibbonNext(void *opaque,uint32_t p){auto &r=*static_cast<RibbonContext*>(opaque);return GuestCall{r.cpu}(r.next_fn,{r.manager,p});}
void RibbonPair(void *opaque,uint32_t p,int origin,float out[2][3]){
    auto &r=*static_cast<RibbonContext*>(opaque);GuestCall game{r.cpu};if(!p)p=game.Call<0x800B5AC8>({r.manager});game(r.direction_fn,{r.direction_out,r.context,p});uint32_t position=origin?r.context+0xa4:p+0xac;
    game.Call<0x800B584C>({r.orientation,r.strategy,r.context,r.flags,p,r.direction_out,position});game.Call<0x800B5334>({r.size_matrix,r.strategy,p},{r.pivot});float orientation[12],size[12],matrix[12];ReadVector(r.orientation,orientation,12);ReadVector(r.size_matrix,size,12);
    for(uint32_t row=0;row<3;row++)for(uint32_t col=0;col<4;col++)matrix[4*row+col]=orientation[4*row]*size[col]+orientation[4*row+1]*size[4+col]+orientation[4*row+2]*size[8+col]+(col==3?orientation[4*row+3]:0);
    for(uint32_t side=0;side<2;side++)for(uint32_t row=0;row<3;row++)out[side][row]=((matrix[4*row]*r.axes[side][0]+matrix[4*row+1]*r.axes[side][1])+matrix[4*row+2]*r.axes[side][2])+matrix[4*row+3];
}
void RibbonSubmit(void *opaque,const mh3_effect_quad *q,const float uv[4][2]){auto &r=*static_cast<RibbonContext*>(opaque);Emit(*q,(r.flags&1)!=0,uv,0,false,true);}
}
extern "C" void MH3_EffectRibbonDraw(CpuContext *cpu){
    GuestFrame frame(cpu,0xc0);RibbonContext r{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(5),0,0,0,FlatRead32(frame.Arg(4)),frame[0x10],frame[0x40],frame[0x70],0,{}};GuestCall game{cpu};uint32_t resource=FlatRead32(r.manager+0x24)+0x9c,count=game.Call<0x800B5BB8>({r.strategy,r.manager});
    r.direction_fn=game.Virtual<0x18>(r.strategy,{r.manager});uint32_t order=FlatRead16(resource)&0x800;r.first_fn=game.Virtual<0x10>(r.strategy,{order});r.next_fn=game.Virtual<0x14>(r.strategy,{order});r.pivot=FlatReadFloat32(0x807985e0)*float(int8_t(FlatRead8(resource+0xa9)));ReadVector(frame.Arg(6),r.axes[0],3);ReadVector(frame.Arg(7),r.axes[1],3);
    mh3_effect_ribbon_source source{&r,RibbonFirst,RibbonNext,RibbonPair,RibbonSubmit,count,uint8_t(FlatRead8(resource+0xb2)&7),uint8_t(order!=0),uint8_t(game.Call<0x800B5BAC>({r.strategy,resource})==0x40)};mh3_effect_draw_ribbon(&source);
}
extern "C" void MH3_EffectTubeSegment(CpuContext *cpu){
    mh3_effect_tube_frame frames[2];const uint32_t pointers[2]={cpu->gpr[4],cpu->gpr[5]};for(uint32_t i=0;i<2;i++){ReadVector(pointers[i],frames[i].center,3);ReadVector(pointers[i]+0xc,frames[i].axis_y,3);ReadVector(pointers[i]+0x18,frames[i].axis_z,3);frames[i].texture_t=FlatReadFloat32(pointers[i]+0x24);}uint32_t flags=cpu->gpr[6],segments=cpu->gpr[7],angles=cpu->gpr[8];if(!segments||segments>32766)throw std::runtime_error("Invalid effect tube tessellation");
    for(uint32_t i=0;i<segments;i++){float samples[2][2];ReadVector(angles+8*i,samples[0],2);ReadVector(angles+8*(i+1),samples[1],2);mh3_effect_quad q;float uv[4][2];mh3_effect_tube_quad(frames,samples,i,segments,(flags&16)!=0,&q,uv);Emit(q,(flags&1)!=0,uv,0,false,true);}
}
REGISTER_NATIVE_FUNCTION_AS(0x800B4C08,MH3_EffectRibbonDraw,"EffectRibbonDraw");
REGISTER_NATIVE_FUNCTION_AS(0x800B61A8,MH3_EffectTubeSegment,"EffectTubeSegment");

namespace {
void Concat(const float a[12],const float b[12],float out[12]) {
    for(uint32_t r=0;r<3;r++)for(uint32_t col=0;col<4;col++)out[4*r+col]=a[4*r]*b[col]+a[4*r+1]*b[4+col]+a[4*r+2]*b[8+col]+(col==3?a[4*r+3]:0);
}
void StripeStrategy(CpuContext *cpu,bool smooth) {
    GuestFrame frame(cpu,0x280);uint32_t strategy=frame.Arg(3),info=frame.Arg(4),manager=frame.Arg(5),resource=FlatRead32(manager+0x24)+0x9c;GuestCall game{cpu};
    uint32_t count=game.Call<0x800B5BB8>({strategy,manager});if(!count)return;
    uint32_t context=frame[0x70],initial=frame[0x150],inverse=frame[0x160],direction=frame[0x1a0],orientation=frame[0x1b0],axis0=frame[0x1f0],axis1=frame[0x200];
    game.Call<0x800B7E14>({info,frame[0x10]});
    if(smooth){game.Call<0x800C4D50>({context,frame[0x10],manager});game.Call<0x800C474C>({initial,strategy,resource,context});}
    else{game.Call<0x800B88EC>({context,frame[0x10],manager});game.Call<0x800B81E8>({initial,strategy,resource,context});}
    float history[3];ReadVector(initial,history,3);float sentinel=FlatReadFloat32(smooth?0x80798684:0x807985e4);
    for(uint32_t p=game.Call<0x800B5BA4>({manager});p&&FlatReadFloat32(p+0xcc)>sentinel;p=game.Call<0x800B5B98>({manager,p}))WriteVector(p+0xcc,history,3);
    uint32_t order=FlatRead16(resource)&0x800,flags=(FlatRead8(resource+0xad)==2?8:0)|(order?16:0);
    float view[12],local[12],combined[12];ReadVector(frame[0x10],view,12);ReadVector(context+0x38,local,12);Concat(view,local,combined);WriteVector(frame[0x40],combined,12);
    if(flags&8){float camera[3]={0,0,1};if(game.Call<0x80088428>({inverse,frame[0x40]}))for(uint32_t r=0;r<3;r++)camera[r]=FlatReadFloat32(inverse+16*r+8);WriteVector(context+0xc8,camera,3);}
    uint32_t closure=smooth?(FlatRead8(resource+0xb2)&7):((FlatRead16(resource+0xb2)>>8)&7);
    uint32_t first_fn=game.Virtual<0x10>(strategy,{order}),next_fn=game.Virtual<0x14>(strategy,{order});
    if((closure==1&&count<3)||(closure==0&&count<2)) {
        uint32_t direction_fn=game.Virtual<0x18>(strategy,{manager});
        for(uint32_t particle=game(first_fn,{manager});particle;particle=game(next_fn,{manager,particle})) {
            game(direction_fn,{direction,context,particle});
            if(smooth)game.Call<0x800C05D8>({orientation,strategy,context,flags,particle,direction,particle+0xac});
            else game.Call<0x800B584C>({orientation,strategy,context,flags,particle,direction,particle+0xac});
        }
        return;
    }
    TextureMaps(strategy,resource);Material(strategy,resource,info);Channels(strategy,resource,info);SetNativePositionMatrix(combined);flags|=effect.resource.texture_count!=0;
    uint32_t particle=game(first_fn,{manager});ParticleState(cpu,strategy,particle,resource,info,1);
    uint32_t subdivisions=smooth?FlatRead8(resource+0xb1):1;if(!subdivisions)subdivisions=1;
    if(smooth) {
        const uint32_t vectors[4]={0x80696a38,0x80696a44,0x80696a50,0x80696a5c},ready[4]={0x80796de8,0x80796de9,0x80796dea,0x80796deb};
        const float axes[4][3]={{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
        for(uint32_t i=0;i<4;i++)if(!FlatRead8(ready[i])){WriteVector(vectors[i],axes[i],3);FlatWrite8(ready[i],1);}
    }
    if(FlatRead8(resource+0xad)==3) {
        uint32_t padding=(FlatRead16(resource)&8)!=0;
        if(!padding){uint32_t owner=FlatRead32(particle+0xc8);owner=FlatRead32(owner+0x20);owner=FlatRead32(owner+0xbc);owner=FlatRead32(owner+0x20);padding=FlatRead8(owner+0xc064)==0;}
        if(smooth)game.Call<0x800C3908>({strategy,context,subdivisions,flags,padding});
        else game.Call<0x800B7254>({strategy,context,flags,padding});
    } else {
        const float axes[4][3]={{1,0,0},{-1,0,0},{0,0,-1},{0,0,1}};
        for(uint32_t plane=0;plane<1u+(FlatRead8(resource+0xad)==1);plane++) {
            if(smooth)game.Call<0x800C279C>({strategy,context,subdivisions,flags,plane?0x80696a50u:0x80696a38u,plane?0x80696a5cu:0x80696a44u});
            else{WriteVector(axis0,axes[2*plane],3);WriteVector(axis1,axes[2*plane+1],3);game.Call<0x800B4C08>({strategy,context,flags,axis0,axis1});}
        }
    }
}
struct SmoothRibbonContext {RibbonContext ribbon;uint32_t output,axis_addresses[2];};
void SmoothPair(void *opaque,uint32_t particle,int,float out[2][3]) {
    auto &s=*static_cast<SmoothRibbonContext*>(opaque);auto &r=s.ribbon;GuestCall game{r.cpu};
    /* C0038 is a CPU-only orientation/size producer. Its ninth integer input
     * is the second axis, passed in the original ABI's stack argument slot. */
    FlatWrite32(r.cpu->gpr[1]+8,s.axis_addresses[1]);
    game.Call<0x800C0038>({r.strategy,s.output,r.context,r.flags,particle,r.direction_fn,particle+0xac,s.axis_addresses[0]},{r.pivot,0});
    ReadVector(s.output,out[0],3);ReadVector(s.output+12,out[1],3);
}
void SmoothSubmit(void *opaque,const mh3_effect_quad *quad,const float uv[4][2]) {auto &s=*static_cast<SmoothRibbonContext*>(opaque);RibbonSubmit(&s.ribbon,quad,uv);}
uint32_t SmoothFirst(void *opaque){return RibbonFirst(&static_cast<SmoothRibbonContext*>(opaque)->ribbon);}
uint32_t SmoothNext(void *opaque,uint32_t particle){return RibbonNext(&static_cast<SmoothRibbonContext*>(opaque)->ribbon,particle);}
void SmoothRibbon(CpuContext *cpu,uint8_t closure) {
    GuestFrame frame(cpu,0xc0);SmoothRibbonContext s{{cpu,frame.Arg(3),frame.Arg(4),frame.Arg(6),0,0,0,FlatRead32(frame.Arg(4)),0,0,0,0,{}},frame[0x20],{frame.Arg(7),frame.Arg(8)}};auto &r=s.ribbon;GuestCall game{cpu};uint32_t resource=FlatRead32(r.manager+0x24)+0x9c;
    r.direction_fn=game.Virtual<0x18>(r.strategy,{r.manager});uint32_t order=FlatRead16(resource)&0x800;r.first_fn=game.Virtual<0x10>(r.strategy,{order});r.next_fn=game.Virtual<0x14>(r.strategy,{order});r.pivot=FlatReadFloat32(0x80798698)*float(int8_t(FlatRead8(resource+0xa9)));
    mh3_effect_smooth_ribbon_source source{};source.ribbon={&s,SmoothFirst,SmoothNext,SmoothPair,SmoothSubmit,game.Call<0x800B5BB8>({r.strategy,r.manager}),closure,uint8_t(order!=0),uint8_t((FlatRead8(resource+0xb2)&0xc0)==0x40)};source.subdivisions=frame.Arg(5);
    if(closure==2){uint32_t youngest=game.Call<0x800B5AC8>({r.manager});if(!youngest)throw std::runtime_error("Smooth stripe origin has no live particle");for(uint32_t axis=0;axis<3;axis++)source.origin_delta[axis]=FlatReadFloat32(r.context+0xa4+4*axis)-FlatReadFloat32(youngest+0xac+4*axis);}
    mh3_effect_draw_smooth_ribbon(&source);
}
void ReadTubeFrame(uint32_t pointer,mh3_effect_tube_frame &frame){ReadVector(pointer,frame.center,3);ReadVector(pointer+12,frame.axis_y,3);ReadVector(pointer+24,frame.axis_z,3);frame.texture_t=FlatReadFloat32(pointer+36);}
}
extern "C" void MH3_EffectStripeStrategy(CpuContext *cpu){StripeStrategy(cpu,false);}
extern "C" void MH3_EffectSmoothStripeStrategy(CpuContext *cpu){StripeStrategy(cpu,true);}
extern "C" void MH3_EffectSmoothOpen(CpuContext *cpu){SmoothRibbon(cpu,0);}
extern "C" void MH3_EffectSmoothClosed(CpuContext *cpu){SmoothRibbon(cpu,1);}
extern "C" void MH3_EffectSmoothOrigin(CpuContext *cpu){SmoothRibbon(cpu,2);}
extern "C" void MH3_EffectSmoothTubeSegment(CpuContext *cpu) {
    uint32_t subdivisions=cpu->gpr[4],segments=cpu->gpr[5],flags=cpu->gpr[9],angles=cpu->gpr[10];
    if(!subdivisions||!segments||segments>32766)throw std::runtime_error("Invalid smooth effect tube tessellation");
    mh3_effect_tube_frame control[3],previous,current,frames[2];for(uint32_t i=0;i<3;i++)ReadTubeFrame(cpu->gpr[6+i],control[i]);float weights[3];mh3_effect_spline_weights(0,weights);mh3_effect_spline_tube(control,weights,&previous);float step=1.0f/float(subdivisions);
    for(uint32_t span=1;span<=subdivisions;span++) {
        mh3_effect_spline_weights(float(span)*step,weights);mh3_effect_spline_tube(control,weights,&current);
        /* Original normal order emits the next ring first. */
        frames[0]=current;frames[1]=previous;
        for(uint32_t i=0;i<segments;i++){float samples[2][2];ReadVector(angles+8*i,samples[0],2);ReadVector(angles+8*(i+1),samples[1],2);mh3_effect_quad quad;float uv[4][2];mh3_effect_tube_quad(frames,samples,i,segments,(flags&16)!=0,&quad,uv);Emit(quad,(flags&1)!=0,uv,0,false,true);}
        previous=current;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x800B7728,MH3_EffectStripeStrategy,"EffectStripeStrategy");
REGISTER_NATIVE_FUNCTION_AS(0x800C3DEC,MH3_EffectSmoothStripeStrategy,"EffectSmoothStripeStrategy");
REGISTER_NATIVE_FUNCTION_AS(0x800C179C,MH3_EffectSmoothOpen,"EffectSmoothOpen");
REGISTER_NATIVE_FUNCTION_AS(0x800C1C30,MH3_EffectSmoothClosed,"EffectSmoothClosed");
REGISTER_NATIVE_FUNCTION_AS(0x800C2148,MH3_EffectSmoothOrigin,"EffectSmoothOrigin");
REGISTER_NATIVE_FUNCTION_AS(0x800C1084,MH3_EffectSmoothTubeSegment,"EffectSmoothTubeSegment");
extern "C" void MH3_NativeMovieCleanup(CpuContext *cpu) {
    GuestFrame frame(cpu,0x20);GuestCall game{cpu};game.Call<0x804E91A0>({1});game.Call<0x804E9010>();game.Call<0x804E8050>();game.Call<0x80523B10>();game.Call<0x8046DAA0>({0});game.Call<0x804DF7B0>();game.Call<0x804DDAD0>();game.Call<0x804C2E80>();
    /* Only the removed legacy GX completion callbacks were cleared here.
     * All CPU/VI cleanup is retained; native rendering has no installed GX callback. */
    game.Call<0x804FD8E0>({0});game.Call<0x804FD9A0>({0});game.Call<0x804E7790>({0});
}
REGISTER_NATIVE_FUNCTION_AS(0x804360B0,MH3_NativeMovieCleanup,"NativeMovieCleanup");
extern "C" void MH3_EffectLineLeaf(CpuContext *cpu) {
    if(!effect.line_size)throw std::runtime_error("Effect line leaf has unresolved width");
    mh3_effect_quad q;for(uint32_t i=0;i<4;i++)ReadVector(cpu->gpr[i<2?3:4],q.position[i],3);
    const float uv[4][2]={{0,0},{0,0},{0,1},{0,1}};Emit(q,cpu->gpr[5]!=0,uv,effect.line_size,false);
}
extern "C" void MH3_EffectPointLeaf(CpuContext *cpu) {
    if(!effect.point_size)throw std::runtime_error("Effect point leaf has unresolved width");
    mh3_effect_quad q;for(uint32_t i=0;i<4;i++)ReadVector(cpu->gpr[3],q.position[i],3);
    const float uv[4][2]={};Emit(q,cpu->gpr[4]!=0,uv,effect.point_size,true);
}
REGISTER_NATIVE_FUNCTION_AS(0x800BF52C,MH3_EffectLineLeaf,"EffectLineLeaf");
REGISTER_NATIVE_FUNCTION_AS(0x800BFD30,MH3_EffectPointLeaf,"EffectPointLeaf");
