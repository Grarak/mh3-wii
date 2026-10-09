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
void Emit(const mh3_effect_quad &q,bool textured,const float (*coordinates)[2]=nullptr,uint32_t size_code=0,bool point=false){
    AuroraNativeMaterialGeometry geometry{};if(!aurora_allocate_native_material_geometry(4,6,&geometry))return;
    const float defaults[4][2]={{0,1},{0,0},{1,0},{1,1}},zero[4][2]={};const auto &material=GetNativeMaterialState();mh3_effect_vertex_inputs input{};
    std::memcpy(input.texture_matrix,effect.texture_matrix,sizeof(input.texture_matrix));for(uint32_t i=0;i<8;i++)input.position_source[i]=effect.position_source[i];input.normal[0]=1;float normal_matrix[9];if(GetNativeNormalMatrix(0,normal_matrix)){for(uint32_t r=0;r<3;r++)input.normal[r]=normal_matrix[3*r];float n=std::sqrt(input.normal[0]*input.normal[0]+input.normal[1]*input.normal[1]+input.normal[2]*input.normal[2]);if(n>1e-5f)for(float &v:input.normal)v/=n;}else if(material.channel[0].color.lighting||material.channel[0].alpha.lighting){throw std::runtime_error("Lit effect has unresolved inherited normal matrix0");}for(uint32_t c=0;c<2;c++)std::memcpy(input.color[c],material.channel[c].material_color,4);
    mh3_effect_material_vertices(&GetNativeDrawState(),&q,coordinates?coordinates:textured?defaults:zero,&input,geometry.vertices);
    if(size_code){
        if(point)mh3_effect_expand_point(&GetNativeDrawState(),float(size_code)/6,geometry.vertices);else mh3_effect_expand_line(&GetNativeDrawState(),float(size_code)/6,geometry.vertices);
        for(uint32_t i=0;i<4;i++){if(point&&(i&1))geometry.vertices[i].texcoord[0][0]+=1;if(point&&i>=2)geometry.vertices[i].texcoord[0][1]+=1;if(!point&&(i&1))geometry.vertices[i].texcoord[0][1]+=1;}mh3_effect_strip_indices(4,geometry.indices);
    }else mh3_effect_quad_indices(0,geometry.indices);
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
        if(force){mh3_native_material_state material=GetNativeMaterialState();auto &gen=material.texgen[map];gen.known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;std::memset(gen.values,0,sizeof(gen.values));gen.values[MH3_TEXGEN_PROJECTION]=t.position_source?0:1;gen.values[MH3_TEXGEN_SOURCE_ROW]=t.position_source?0:5;gen.values[MH3_TEXGEN_POST_MATRIX_ROW]=t.position_source?uint8_t(3*map):61;SetNativeMaterial(material);}
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
    uint32_t index=c->gpr[3];if(index>=8)throw std::runtime_error("Effect texgen index exceeds material layout");
    auto material=GetNativeMaterialState();auto &gen=material.texgen[index];gen.known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;std::memset(gen.values,0,sizeof(gen.values));gen.values[MH3_TEXGEN_PROJECTION]=c->gpr[4]==1;gen.values[MH3_TEXGEN_SOURCE_ROW]=c->gpr[5]==4?5:uint8_t(c->gpr[5]);gen.values[MH3_TEXGEN_MATRIX_ROW]=uint8_t(c->gpr[6]);gen.values[MH3_TEXGEN_NORMALIZE]=c->gpr[7]!=0;gen.values[MH3_TEXGEN_POST_MATRIX_ROW]=uint8_t(c->gpr[8]-0x40);SetNativeMaterial(material);
}
REGISTER_NATIVE_FUNCTION_AS(0x804351C4,MH3_NativeMovieTexture,"NativeMovieTexture");
REGISTER_NATIVE_FUNCTION_AS(0x804352C8,MH3_NativeMovieRestore,"NativeMovieRestore");
REGISTER_NATIVE_FUNCTION_AS(0x804353E0,MH3_NativeMovieSetup,"NativeMovieSetup");
REGISTER_NATIVE_FUNCTION_AS(0x80435894,MH3_NativeMoviePlanes,"NativeMoviePlanes");
REGISTER_NATIVE_FUNCTION_AS(0x80435888,MH3_EffectTexCoordGenerator,"NativeMovieTexgen");
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
    bool first=s.last_size==0;ParticleState(c.cpu,c.strategy,id,c.resource,c.info,uint32_t(first));s.last_size=code;Emit(q,effect.resource.texture_count!=0,s.point?point_uv:line_uv,code,s.point);
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
