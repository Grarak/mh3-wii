// Typed native material state, pure equation generation, and direct frame-slot
// uniforms. No GX state, command-list decoder, FIFO, or GPU wait participates.
#include "native_graphics.h"
#include "model_assets.h"
#include "../native/mh3_material_state.h"
#include "../native/mh3_model_gpu_inputs.h"
#include "../native/adapters/model_gpu_bridge.h"
#include "../native/adapters/model_pose_bridge.h"
#include <aurora/native_texture.hpp>
#include "gx/gx.hpp"
#include "gx/shader_info.hpp"
#include "gl/gl.hpp"
#ifdef MH3_DEVELOPMENT
#include "gfx/port_capture.hpp"
namespace aurora::gfx { uint32_t current_frame() noexcept; }
#endif
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace {
using aurora::gx::ShaderConfig;
using aurora::gx::ShaderInfo;
constexpr uint64_t Bit(unsigned field) {return UINT64_C(1)<<field;}
constexpr uint64_t AlphaFields=Bit(MH3_PIXEL_ALPHA_COMPARE0)|Bit(MH3_PIXEL_ALPHA_COMPARE1)|Bit(MH3_PIXEL_ALPHA_OPERATION);
mh3_native_material_state material{};
mh3_native_fog fogSource{};
mh3_shader_fog preparedFog{};
bool fogKnown=false,depthBeforeTexture=false;
mh3_native_light_slots lightSlots{};
std::array<mh3_shader_light,8> preparedLights{};
struct TextureMatrix {std::array<float,12> values{};bool known=false,matrix3x4=false;};
std::array<TextureMatrix,10> textureMatrices{};
std::array<TextureMatrix,20> postTextureMatrices{};
struct PreparedMaterial {ShaderConfig config;ShaderInfo info;mh3_material_requirements requirements{};};
PreparedMaterial prepared;
bool preparedKnown=false;
bool rasterObserverInstalled=false,publishingMaterialRaster=false;
struct ProgramKey {ShaderConfig config;uint8_t zeroBias;};
struct ProgramOrder {
    bool operator()(const ProgramKey& a,const ProgramKey& b) const {
        const int order=std::memcmp(&a.config,&b.config,sizeof a.config);
        return order<0 || (order==0 && a.zeroBias<b.zeroBias);
    }
};
// Ordered typed configuration keys avoid reading or hashing asset contents.
std::map<ProgramKey,aurora::gl::ProgramPtr,ProgramOrder> programs;
struct ModelProgramKey {
    ProgramKey material;
    std::array<uint32_t,17> layout{};
    uint8_t nonzeroProjection=0;
    uint8_t zeroProjection=0;
};
struct ModelProgramOrder {
    bool operator()(const ModelProgramKey& a,const ModelProgramKey& b) const {
        const ProgramOrder compare;
        if(compare(a.material,b.material)) return true;
        if(compare(b.material,a.material)) return false;
        if(a.layout!=b.layout) return a.layout<b.layout;
        if(a.nonzeroProjection!=b.nonzeroProjection) return a.nonzeroProjection<b.nonzeroProjection;
        return a.zeroProjection<b.zeroProjection;
    }
};
std::map<ModelProgramKey,aurora::gl::ProgramPtr,ModelProgramOrder> modelPrograms;
void Owner() {
    if(!aurora::gl::on_render_thread()) throw std::runtime_error("Native material changed outside graphics owner");
}
void Check(mh3_res_status status,const char* operation) {
    if(status!=MH3_RES_OK) throw std::runtime_error(std::string(operation)+": status "+std::to_string(status));
}
void Validate(const mh3_material_requirements* requirements) {
    mh3_material_diagnostic diagnostic{};
    // PrepareMaterial checks all domains once before building dependencies.
    // Its owner-only state remains unchanged throughout that preparation.
    const auto status=requirements ? mh3_material_state_require(&material,requirements,&diagnostic) :
        mh3_material_state_validate(&material,nullptr,&diagnostic);
    if(status!=MH3_RES_OK) throw std::runtime_error("Native material status "+std::to_string(status)+
        ", group "+std::to_string(diagnostic.group)+", index "+std::to_string(diagnostic.index)+
        ", fields "+std::to_string(diagnostic.fields));
}
void ImportRaster(const mh3_raster_state& raster) {
    const uint32_t fields[][2]={
        {MH3_PIXEL_DEPTH_TEST,raster.depth_test},{MH3_PIXEL_DEPTH_COMPARE,raster.depth_compare},
        {MH3_PIXEL_DEPTH_WRITE,raster.depth_write},{MH3_PIXEL_BLEND_ENABLE,raster.blend_mode==1},
        {MH3_PIXEL_LOGIC_ENABLE,raster.blend_mode==2},{MH3_PIXEL_BLEND_SUBTRACT,raster.blend_mode==3},
        {MH3_PIXEL_BLEND_SOURCE,raster.src_factor},{MH3_PIXEL_BLEND_DESTINATION,raster.dst_factor},
        {MH3_PIXEL_LOGIC_OPERATION,raster.logic_op},{MH3_PIXEL_COLOR_WRITE,raster.color_write},
        {MH3_PIXEL_ALPHA_WRITE,raster.alpha_write}};
    for(const auto& field:fields) {
        material.pixel.known|=Bit(field[0]);material.pixel.values[field[0]]=field[1];
    }
    material.cull_mode=raster.cull_mode;
}
void ObserveRaster(const mh3_raster_state& raster) {
    if(!publishingMaterialRaster) ImportRaster(raster);
}
void ObserveRasterPublications() {
    if(rasterObserverInstalled) return;
    SetNativeRasterObserver(ObserveRaster);rasterObserverInstalled=true;
    ImportRaster(GetNativeDrawState().raster);
}
bool ColorUses(const mh3_pack_tev_stage& stage,uint8_t arg) {
    return stage.color.a==arg || stage.color.b==arg || stage.color.c==arg || stage.color.d==arg;
}
bool AlphaUses(const mh3_pack_tev_stage& stage,uint8_t arg) {
    return stage.alpha.a==arg || stage.alpha.b==arg || stage.alpha.c==arg || stage.alpha.d==arg;
}
uint64_t PassFields(const mh3_pack_tev_pass& pass,unsigned base) {
    // Comparisons do not consume the regular equation's bias and scale.
    uint64_t mask=(UINT64_C(511)<<base);
    if(pass.operation>=8) mask&=~(Bit(base+5)|Bit(base+6));
    return mask;
}
void ColorPass(const mh3_pack_tev_pass& from,aurora::gx::TevStage& to) {
    to.colorPass={GXTevColorArg(from.a),GXTevColorArg(from.b),GXTevColorArg(from.c),GXTevColorArg(from.d)};
    to.colorOp.op=GXTevOp(from.operation);to.colorOp.clamp=from.clamp;to.colorOp.outReg=GXTevRegID(from.output);
    if(from.operation<=1) {to.colorOp.bias=GXTevBias(from.bias);to.colorOp.scale=GXTevScale(from.scale);}
}
void AlphaPass(const mh3_pack_tev_pass& from,aurora::gx::TevStage& to) {
    to.alphaPass={GXTevAlphaArg(from.a),GXTevAlphaArg(from.b),GXTevAlphaArg(from.c),GXTevAlphaArg(from.d)};
    to.alphaOp.op=GXTevOp(from.operation);to.alphaOp.clamp=from.clamp;to.alphaOp.outReg=GXTevRegID(from.output);
    if(from.operation<=1) {to.alphaOp.bias=GXTevBias(from.bias);to.alphaOp.scale=GXTevScale(from.scale);}
}
void KonstColor(uint8_t selection,mh3_material_requirements& requirements) {
    if(selection>=12 && selection<16) requirements.konst[selection-12]|=7;
    else if(selection>=16) requirements.konst[(selection-16)%4]|=1u<<((selection-16)/4);
}
void KonstAlpha(uint8_t selection,mh3_material_requirements& requirements) {
    if(selection>=16) requirements.konst[(selection-16)%4]|=1u<<((selection-16)/4);
}
unsigned Channel(uint8_t semantic) {
    return semantic==1 || semantic==3 || semantic==5 ? 1 : 0;
}
void ChannelConfig(const mh3_pack_channel_control& source,aurora::gx::ColorChannelConfig& out) {
    out.matSrc=GXColorSrc(source.material_source);out.ambSrc=GXColorSrc(source.ambient_source);
    out.lightingEnabled=source.lighting;out.diffFn=GXDiffuseFn(source.diffuse);out.attnFn=GXAttnFn(source.attenuation);
}
PreparedMaterial PrepareMaterial() {
    Validate(nullptr);
    if(!material.tev_count) throw std::runtime_error("Native material has no active combiner stages");
    if(material.fog_reference>=0 && !fogKnown) throw std::runtime_error("Native material fog reference has no typed producer");
    PreparedMaterial result;
    auto& config=result.config;auto& requirements=result.requirements;
    config.vtxStride=sizeof(AuroraNativeMaterialVertex);
    config.attrs[GX_VA_POS]={GX_DIRECT,3,GX_F32};config.attrs[GX_VA_NRM]={GX_DIRECT,3,GX_F32};
    config.attrs[GX_VA_CLR0]={GX_DIRECT,4,GX_RGBA8};config.attrs[GX_VA_CLR1]={GX_DIRECT,4,GX_RGBA8};
    for(unsigned i=0;i<8;++i) config.attrs[GX_VA_TEX0+i]={GX_DIRECT,2,GX_F32};
    config.tevStageCount=material.tev_count;config.numTexGens=material.texgen_count;
    config.numIndStages=material.indirect_count;
    requirements.pixel=AlphaFields;
    // An unknown operation must be diagnosed before it can narrow dependencies.
    for(unsigned i=0;i<material.tev_count;++i) requirements.tev[i]=Bit(MH3_TEV_COLOR_OPERATION)|Bit(MH3_TEV_ALPHA_OPERATION);
    Validate(&requirements);
    for(unsigned i=0;i<material.tev_count;++i) {
        requirements.tev[i]|=PassFields(material.tev[i].color,0)|PassFields(material.tev[i].alpha,9);
    }
    Validate(&requirements);
    bool anyTexture=false;
    std::array<uint8_t,2> usedChannelComponents{};
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];auto& stage=config.tevStages[i];
        ColorPass(source.color,stage);AlphaPass(source.alpha,stage);
        const bool textureRgb=ColorUses(source,8),textureAlpha=ColorUses(source,9)||AlphaUses(source,4);
        const bool rasterRgb=ColorUses(source,10),rasterAlpha=ColorUses(source,11)||AlphaUses(source,5);
        if(textureRgb || textureAlpha) requirements.tev[i]|=Bit(MH3_TEV_TEXTURE_MAP);
        if(rasterRgb || rasterAlpha) requirements.tev[i]|=Bit(MH3_TEV_RASTER_CHANNEL);
        if(ColorUses(source,14)) requirements.tev[i]|=Bit(MH3_TEV_KONST_COLOR);
        if(AlphaUses(source,6)) requirements.tev[i]|=Bit(MH3_TEV_KONST_ALPHA);
    }
    Validate(&requirements);
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];
        if((ColorUses(source,8)||ColorUses(source,9)||AlphaUses(source,4)) && source.texture_map!=255 && material.texgen_count)
            anyTexture=true;
    }
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];auto& stage=config.tevStages[i];
        const bool textureRgb=ColorUses(source,8),textureAlpha=ColorUses(source,9)||AlphaUses(source,4);
        const bool rasterRgb=ColorUses(source,10),rasterAlpha=ColorUses(source,11)||AlphaUses(source,5);
        if(textureRgb || textureAlpha) {
            stage.texMapId=GXTexMapID(source.texture_map);
            if(source.texture_map!=255 && material.texgen_count) {
                requirements.tev[i]|=Bit(MH3_TEV_TEXTURE_COORDINATE)|Bit(MH3_TEV_TEXTURE_SWAP);
            }
        }
        if(rasterRgb || rasterAlpha) {
            stage.channelId=GXChannelID(source.raster_channel);
            if(source.raster_channel<=5) requirements.tev[i]|=Bit(MH3_TEV_RASTER_SWAP);
            if(source.raster_channel==7 || source.raster_channel==8) requirements.tev[i]|=Bit(MH3_TEV_INDIRECT_ALPHA);
        }
        if(ColorUses(source,14)) {stage.kcSel=GXTevKColorSel(source.konst_color);KonstColor(source.konst_color,requirements);}
        if(AlphaUses(source,6)) {stage.kaSel=GXTevKAlphaSel(source.konst_alpha);KonstAlpha(source.konst_alpha,requirements);}
        if(anyTexture) requirements.tev[i]|=Bit(MH3_TEV_INDIRECT_MATRIX)|Bit(MH3_TEV_INDIRECT_WRAP_S)|
            Bit(MH3_TEV_INDIRECT_WRAP_T)|Bit(MH3_TEV_INDIRECT_ADD_PREVIOUS);
    }
    Validate(&requirements);
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];auto& stage=config.tevStages[i];
        const bool textureRgb=ColorUses(source,8),textureAlpha=ColorUses(source,9)||AlphaUses(source,4);
        const bool rasterRgb=ColorUses(source,10),rasterAlpha=ColorUses(source,11)||AlphaUses(source,5);
        if(anyTexture) {
            stage.indTexMtxId=GXIndTexMtxID(source.indirect_matrix);stage.indTexWrapS=GXIndTexWrap(source.indirect_wrap_s);
            stage.indTexWrapT=GXIndTexWrap(source.indirect_wrap_t);stage.indTexAddPrev=source.indirect_add_previous;
        }
        if((textureRgb||textureAlpha) && source.texture_map!=255 && material.texgen_count) {
            stage.texCoordId=GXTexCoordID(source.texture_coordinate);stage.tevSwapTex=GXTevSwapSel(source.texture_swap);
            requirements.swaps[source.texture_swap]|=(textureRgb?7u:0u)|(textureAlpha?8u:0u);
        }
        if((rasterRgb||rasterAlpha) && source.raster_channel<=5) {
            stage.tevSwapRas=GXTevSwapSel(source.raster_swap);
            requirements.swaps[source.raster_swap]|=(rasterRgb?7u:0u)|(rasterAlpha?8u:0u);
        }
        if(source.raster_channel==7 || source.raster_channel==8) {
            if(rasterRgb||rasterAlpha) {
                stage.indTexAlphaSel=GXIndTexAlphaSel(source.indirect_alpha);
                if(source.indirect_alpha) requirements.tev[i]|=Bit(MH3_TEV_INDIRECT_STAGE)|Bit(MH3_TEV_INDIRECT_FORMAT);
                else stage.channelId=GX_COLOR_ZERO;
            }
        }
        if(stage.indTexMtxId!=GX_ITM_OFF) requirements.tev[i]|=Bit(MH3_TEV_INDIRECT_STAGE);
    }
    Validate(&requirements);
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];auto& stage=config.tevStages[i];
        if(requirements.tev[i]&Bit(MH3_TEV_INDIRECT_STAGE)) stage.indTexStage=GXIndTexStageID(source.indirect_stage);
        const bool bump=stage.channelId==GX_ALPHA_BUMP || stage.channelId==GX_ALPHA_BUMPN;
        if((stage.indTexMtxId!=GX_ITM_OFF || (bump && stage.indTexAlphaSel!=GX_ITBA_OFF)) && stage.indTexStage<material.indirect_count) {
            requirements.tev[i]|=Bit(MH3_TEV_INDIRECT_FORMAT)|Bit(MH3_TEV_INDIRECT_BIAS)|Bit(MH3_TEV_INDIRECT_ALPHA)|Bit(MH3_TEV_INDIRECT_ORIGINAL_LOD);
            requirements.indirect[stage.indTexStage]=15;
        }
        if(aurora::gx::tev_stage_needs_fixed_texcoord_state(config,i) && material.texgen_count)
            requirements.tev[i]|=Bit(MH3_TEV_TEXTURE_COORDINATE);
    }
    Validate(&requirements);
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& source=material.tev[i];auto& stage=config.tevStages[i];
        if(requirements.tev[i]&Bit(MH3_TEV_TEXTURE_COORDINATE)) stage.texCoordId=GXTexCoordID(source.texture_coordinate);
        if(requirements.tev[i]&Bit(MH3_TEV_INDIRECT_FORMAT)) {
            stage.indTexFormat=GXIndTexFormat(source.indirect_format);stage.indTexAlphaSel=GXIndTexAlphaSel(source.indirect_alpha);
            if(requirements.tev[i]&Bit(MH3_TEV_INDIRECT_BIAS)) stage.indTexBiasSel=GXIndTexBiasSel(source.indirect_bias);
            if(requirements.tev[i]&Bit(MH3_TEV_INDIRECT_ORIGINAL_LOD)) stage.indTexUseOrigLOD=source.indirect_original_lod;
        }
        if(ColorUses(source,10)||ColorUses(source,11)||AlphaUses(source,5)) if(source.raster_channel<=5) {
            const unsigned channel=Channel(source.raster_channel),swap=source.raster_swap;
            const uint32_t outputs=(ColorUses(source,10)?7u:0u)|((ColorUses(source,11)||AlphaUses(source,5))?8u:0u);
            for(unsigned component=0;component<4;++component) if(outputs&(1u<<component)) {
                const uint8_t input=material.swaps[swap].rgba[component];
                usedChannelComponents[channel]|=input==3 ? 8 : 7;
            }
        }
    }
    for(unsigned swap=0;swap<4;++swap) {
        auto& target=config.tevSwapTable[swap];const auto& source=material.swaps[swap];
        // Unconsumed mappings stay canonical and do not read unknown bytes.
        if(requirements.swaps[swap]&1) target.red=GXTevColorChan(source.rgba[0]);
        if(requirements.swaps[swap]&2) target.green=GXTevColorChan(source.rgba[1]);
        if(requirements.swaps[swap]&4) target.blue=GXTevColorChan(source.rgba[2]);
        if(requirements.swaps[swap]&8) target.alpha=GXTevColorChan(source.rgba[3]);
    }
    for(unsigned i=0;i<4;++i) if(requirements.indirect[i]) {
        const auto& source=material.indirect[i];
        if(source.map==255) throw std::runtime_error("Native indirect sampling has a null texture map");
        config.indStages[i]={GXTexCoordID(source.coordinate),GXTexMapID(source.map),GXIndTexScale(source.scale_s),GXIndTexScale(source.scale_t)};
    }
    for(unsigned i=0;i<2;++i) {
        // Native vertices contain both channels. A component never consumed by
        // the equation uses its vertex input to avoid inventing register colors.
        config.colorChannels[i].matSrc=GX_SRC_VTX;config.colorChannels[GX_ALPHA0+i].matSrc=GX_SRC_VTX;
        if(usedChannelComponents[i]&7) requirements.channel[i]|=MH3_CHANNEL_CONTROL_COLOR;
        if(usedChannelComponents[i]&8) requirements.channel[i]|=MH3_CHANNEL_CONTROL_ALPHA;
    }
    Validate(&requirements);
    for(unsigned i=0;i<2;++i) {
        const auto& source=material.channel[i];
        if(usedChannelComponents[i]&7) {
            ChannelConfig(source.color,config.colorChannels[i]);
            if(!source.color.material_source) requirements.channel[i]|=MH3_CHANNEL_MATERIAL_COLOR;
            if(source.color.lighting && !source.color.ambient_source) requirements.channel[i]|=MH3_CHANNEL_AMBIENT_COLOR;
        }
        if(usedChannelComponents[i]&8) {
            ChannelConfig(source.alpha,config.colorChannels[GX_ALPHA0+i]);
            if(!source.alpha.material_source) requirements.channel[i]|=MH3_CHANNEL_MATERIAL_ALPHA;
            if(source.alpha.lighting && !source.alpha.ambient_source) requirements.channel[i]|=MH3_CHANNEL_AMBIENT_ALPHA;
        }
    }
    const auto& pixel=material.pixel.values;
    config.alphaCompare={GXCompare(pixel[MH3_PIXEL_ALPHA_COMPARE0]),0,GXAlphaOp(pixel[MH3_PIXEL_ALPHA_OPERATION]),GXCompare(pixel[MH3_PIXEL_ALPHA_COMPARE1]),0};
    if(pixel[MH3_PIXEL_ALPHA_COMPARE0]!=0 && pixel[MH3_PIXEL_ALPHA_COMPARE0]!=7) requirements.pixel|=Bit(MH3_PIXEL_ALPHA_REFERENCE0);
    if(pixel[MH3_PIXEL_ALPHA_COMPARE1]!=0 && pixel[MH3_PIXEL_ALPHA_COMPARE1]!=7) requirements.pixel|=Bit(MH3_PIXEL_ALPHA_REFERENCE1);
    Validate(&requirements);
    if(requirements.pixel&Bit(MH3_PIXEL_ALPHA_REFERENCE0)) config.alphaCompare.ref0=pixel[MH3_PIXEL_ALPHA_REFERENCE0];
    if(requirements.pixel&Bit(MH3_PIXEL_ALPHA_REFERENCE1)) config.alphaCompare.ref1=pixel[MH3_PIXEL_ALPHA_REFERENCE1];
    if(fogKnown) {config.fogType=fogSource.type;config.fogRangeAdjust=fogSource.range_enabled;}
    // Native STQ is final, so no generated texgen or postmatrix can be consumed.
    result.info=aurora::gx::build_shader_info(config);
    for(unsigned i=0;i<8;++i) if(result.info.sampledTexCoords.test(i)) requirements.texgen[i]=1u<<MH3_TEXGEN_PROJECTION;
    for(unsigned i=0;i<4;++i) {
        if(result.info.loadsTevRegRgb.test(i)) requirements.color[i]|=7;
        if(result.info.loadsTevRegAlpha.test(i)) requirements.color[i]|=8;
    }
    for(unsigned i=0;i<material.tev_count;++i) {
        const auto& stage=config.tevStages[i];const unsigned selection=stage.indTexMtxId;
        if(stage.indTexStage>=config.numIndStages) continue;
        if(selection>=1 && selection<=3) requirements.indirect_matrix[selection-1]|=127;
        else if(selection>=5 && selection<=7) requirements.indirect_matrix[selection-5]|=64;
        else if(selection>=9 && selection<=11) requirements.indirect_matrix[selection-9]|=64;
    }
    Validate(&requirements);
    for(unsigned i=0;i<8;++i) if(result.info.sampledTexCoords.test(i))
        config.tcgs[i].type=material.texgen[i].values[MH3_TEXGEN_PROJECTION] ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
    // Projection changes varying types, but cannot introduce matrix dependencies.
    result.info=aurora::gx::build_shader_info(config);
    return result;
}
const mh3_native_texture_binding& Texture(unsigned unit) {
    const auto* binding=GetNativeTextureBinding(unit);
    if(!binding) throw std::runtime_error("Native material texture "+std::to_string(unit)+" has no typed producer");
    return *binding;
}
std::array<float,4> CoordinateScale(unsigned coordinate,const PreparedMaterial& state) {
    if(!aurora::gx::shader_uses_fixed_texcoord_state(state.config) && !state.info.usedIndStages.any()) return {1,1,0,0};
    std::array<float,4> result{};bool known=false;
    const auto add=[&](unsigned map) {
        const auto& source=Texture(map).source;
        if(known && (result[0]!=source.width || result[1]!=source.height))
            throw std::runtime_error("Native fixed coordinate requires an explicit scale producer for conflicting texture dimensions");
        result={float(source.width),float(source.height),0,0};known=true;
    };
    for(unsigned stage=0;stage<state.config.tevStageCount;++stage) {
        const auto dependency=aurora::gx::tev_stage_texture_dependency(state.config,stage);
        if(dependency.texCoordId==int(coordinate) && dependency.texMapId>=0) add(unsigned(dependency.texMapId));
    }
    for(unsigned stage=0;stage<4;++stage) if(state.info.usedIndStages.test(stage)) {
        const auto& indirect=state.config.indStages[stage];
        if(aurora::gx::tev_effective_texcoord(state.config,indirect.texCoordId)==int(coordinate)) add(indirect.texMapId);
    }
    if(!known) throw std::runtime_error("Native fixed coordinate scale has no typed texture dimensions");
    return result;
}
struct UniformWriter {
    uint8_t* destination;size_t capacity,offset=0;
    void Bytes(const void* value,size_t size) {
        if(size>capacity-offset) throw std::runtime_error("Native material uniform layout overflow");
        std::memcpy(destination+offset,value,size);offset+=size;
    }
    template<typename T> void Value(const T& value) {Bytes(&value,sizeof value);}
};
std::array<float,4> Constant(const mh3_pack_tev_color& color,uint32_t consumed) {
    std::array<float,4> out{};
    for(unsigned component=0;component<4;++component) if(consumed&(1u<<component)) out[component]=color.rgba[component]/255.f;
    return out;
}
std::array<float,4> ChannelColor(const uint8_t rgba[4],bool alpha) {
    std::array<float,4> out{};
    if(alpha) out[3]=rgba[3]/255.f;
    else for(unsigned component=0;component<3;++component) out[component]=rgba[component]/255.f;
    return out;
}
void WriteUniforms(const PreparedMaterial& state,AuroraNativeUniforms& uniforms,
                   const mh3_model_gpu_inputs* model=nullptr,const mh3_model_texcoord_sizes* sizes=nullptr) {
    const auto& info=state.info;const auto& config=state.config;
    // The native vertex shader does not consume the declared GX matrix prefix.
    // Zero unused declarations/padding once; append actual material fields in order.
    std::memset(uniforms.data,0,uniforms.size);
    if(model) {
        UniformWriter prefix{static_cast<uint8_t*>(uniforms.data),uniforms.size};
        prefix.offset=8;
        std::array<float,2> viewport{};GetNativeRenderViewportSize(viewport.data());
        prefix.Value(viewport);
        prefix.Value(std::array<float,2>{GetNativeDrawState().viewport[2],GetNativeDrawState().viewport[3]});
        prefix.offset=80;prefix.Bytes(model->projection,64);
        for(unsigned i=0;i<info.matrixLayout.postexCount;++i) {
            const unsigned slot=info.matrixLayout.postexSlots[i];
            if(slot==aurora::gx::UniformMatrixLayout::kCurrentPnMtx) prefix.Value(model->position);
            else if(info.matrixLayout.absolutePosRegion && slot<10) prefix.Value(model->position_palette[slot]);
            else if(info.matrixLayout.absolutePosRegion && slot<20) prefix.Value(model->texture_palette[slot-10]);
            else if(slot>=10 && slot<18 && (model->texcoord_mask&(1u<<(slot-10)))) prefix.Value(model->texgen[slot-10].main);
            else throw std::runtime_error("Compact model main matrix layout mismatch");
        }
        for(unsigned i=0;i<info.matrixLayout.nrmCount;++i) {
            const unsigned slot=info.matrixLayout.nrmSlots[i];
            if(slot==aurora::gx::UniformMatrixLayout::kCurrentPnMtx) prefix.Value(model->normal);
            else if(info.matrixLayout.absolutePosRegion && slot<10) prefix.Value(model->normal_palette[slot]);
            else throw std::runtime_error("Compact model normal matrix layout mismatch");
        }
    }
    UniformWriter writer{static_cast<uint8_t*>(uniforms.data),uniforms.size,
        144+48u*(info.matrixLayout.postexCount+info.matrixLayout.nrmCount)};
    for(unsigned i=0;i<4;++i) if(info.loadsTevRegRgb.test(i)||info.loadsTevRegAlpha.test(i))
        writer.Value(Constant(material.color[i],state.requirements.color[i]));
    if(info.lightingEnabled) {
        std::array<uint32_t,4> masks{};
        for(unsigned i=0;i<2;++i) {
            if(config.colorChannels[i].lightingEnabled) masks[i]=material.channel[i].color.light_mask;
            if(config.colorChannels[GX_ALPHA0+i].lightingEnabled) masks[GX_ALPHA0+i]=material.channel[i].alpha.light_mask;
        }
        for(unsigned mask:masks) if(mask&~lightSlots.valid_mask)
            throw std::runtime_error("Native material lighting reads an unpublished light slot");
        writer.Value(preparedLights);writer.Value(masks);
    }
    for(unsigned i=0;i<2;++i) if(info.sampledColorChannels.test(i)) {
        const auto& source=material.channel[i];const auto& rgb=config.colorChannels[i];const auto& alpha=config.colorChannels[GX_ALPHA0+i];
        if(rgb.lightingEnabled && rgb.ambSrc==GX_SRC_REG) writer.Value(ChannelColor(source.ambient_color,false));
        if(rgb.matSrc==GX_SRC_REG) writer.Value(ChannelColor(source.material_color,false));
        if(alpha.lightingEnabled && alpha.ambSrc==GX_SRC_REG) writer.Value(ChannelColor(source.ambient_color,true));
        if(alpha.matSrc==GX_SRC_REG) writer.Value(ChannelColor(source.material_color,true));
    }
    for(unsigned i=0;i<4;++i) if(info.sampledKColors.test(i)) writer.Value(Constant(material.konst[i],state.requirements.konst[i]));
    if(info.usesPTTexMtx.any()) {
        if(!model) throw std::runtime_error("Native final STQ unexpectedly requires generated postmatrices");
        for(unsigned i=0;i<aurora::gx::MaxPTTexMtx;++i) {
            if(info.usesPTTexMtx.test(i)) {
                if(i>=8 || !(model->texcoord_mask&(1u<<i))) throw std::runtime_error("Compact model post matrix layout mismatch");
                writer.Value(model->texgen[i].post);
            } else writer.Value(mh3_matrix34{});
        }
    }
    if(info.usesFog) {
        auto fog=preparedFog;Check(mh3_material_prepare_fog(&fogSource,GetNativeDrawState().viewport[2],&fog),"Native fog draw preparation");
        writer.Value(fog);
    }
    if(info.usedIndTexMtxs.any()) for(unsigned i=0;i<3;++i) {
        const auto& matrix=material.indirect_matrix[i];const uint32_t used=state.requirements.indirect_matrix[i];
        std::array<int32_t,8> words{};
        // Source is ordinary row-major 2x3. Shader consumes column pairs.
        if(used&63) words={matrix.coefficients[0],matrix.coefficients[3],matrix.coefficients[1],matrix.coefficients[4],matrix.coefficients[2],matrix.coefficients[5],0,0};
        if(used&64) words[6]=-matrix.scale_exponent;
        writer.Value(words);
    }
    for(unsigned i=0;i<8;++i) if(info.sampledTexCoords.test(i)) {
        if(!model) writer.Value(CoordinateScale(i,state));
        else if(aurora::gx::shader_uses_fixed_texcoord_state(config) || info.usedIndStages.any()) {
            if(!sizes || !(sizes->known&(1u<<i)) || !sizes->width[i] || !sizes->height[i])
                throw std::runtime_error("Compact model lacks an explicit coordinate scale producer");
            writer.Value(std::array<float,4>{float(sizes->width[i]),float(sizes->height[i]),0,0});
        } else writer.Value(std::array<float,4>{1,1,0,0});
    }
    for(unsigned i=0;i<8;++i) if(info.sampledTextures.test(i)) {
        const auto& texture=Texture(i).source;
        writer.Value(std::array<float,4>{float(texture.width),float(texture.height),aurora::native_texture_shader_lod_bias(i),0});
    }
    if(writer.offset>uniforms.size) throw std::runtime_error("Native material uniform size mismatch");
}
}

const mh3_native_material_state& GetNativeMaterialState() {
    Owner();ObserveRasterPublications();return material;
}
void SetNativeMaterial(const mh3_native_material_state& source) {
    OverlayNativeMaterial(source,MH3_MATERIAL_TOP_ALL);
}
void OverlayNativeMaterial(const mh3_native_material_state& source,uint32_t top_known) {
    Owner();ObserveRasterPublications();
    auto next=material;Check(mh3_material_state_overlay(&next,&source,top_known),"Native material overlay");
    auto raster=GetNativeDrawState().raster;Check(mh3_material_pixel_raster(&next.pixel,&raster),"Native material pixel raster");
    raster.cull_mode=next.cull_mode;
    struct Export {Export(){publishingMaterialRaster=true;}~Export(){publishingMaterialRaster=false;}};
    {Export scoped;SetNativeRaster(raster);}
    material=next;preparedKnown=false;
}
void SetNativeFog(const mh3_native_fog& source) {
    Owner();auto next=preparedFog;
    Check(mh3_material_prepare_fog(&source,GetNativeDrawState().viewport[2],&next),"Native fog producer");
    fogSource=source;preparedFog=next;fogKnown=true;preparedKnown=false;
}
void SetNativeModelLights(const mh3_native_light_slots& source) {
    Owner();std::array<mh3_shader_light,8> next{};
    for(unsigned i=0;i<8;++i) if(source.valid_mask&(1u<<i))
        Check(mh3_material_prepare_light(&source.lights[i],&next[i]),"Native light producer");
    lightSlots=source;preparedLights=next;
}
void SetNativeTextureMatrix(uint32_t selector,const float matrix[12],bool matrix3x4) {
    Owner();TextureMatrix* destination=nullptr;
    if(selector>=30 && selector<60 && (selector-30)%3==0) destination=&textureMatrices[(selector-30)/3];
    else if(selector>=64 && selector<124 && (selector-64)%3==0) destination=&postTextureMatrices[(selector-64)/3];
    if(!destination || !matrix) throw std::runtime_error("Invalid native texture-matrix producer");
    for(unsigned i=0;i<12;++i) if(!std::isfinite(matrix[i])) throw std::runtime_error("Nonfinite native texture matrix");
    PublishNativeModelTextureMatrix(selector,matrix,matrix3x4);
    std::memcpy(destination->values.data(),matrix,48);destination->known=true;destination->matrix3x4=matrix3x4;
}
bool GetNativeTextureMatrix(uint32_t selector,float matrix[12],bool* matrix3x4) {
    Owner();const TextureMatrix* source=nullptr;
    if(selector>=30 && selector<60 && (selector-30)%3==0) source=&textureMatrices[(selector-30)/3];
    else if(selector>=64 && selector<124 && (selector-64)%3==0) source=&postTextureMatrices[(selector-64)/3];
    if(!source || !matrix) throw std::runtime_error("Invalid native texture-matrix consumer");
    if(!source->known) return false;
    std::memcpy(matrix,source->values.data(),48);if(matrix3x4) *matrix3x4=source->matrix3x4;
    return true;
}
void SetNativeDepthCompareBeforeTexture(bool before) {Owner();depthBeforeTexture=before;}
void SubmitNativeMaterialGeometry(const AuroraNativeMaterialGeometry& geometry) {
    Owner();if(!NativeGraphicsFrameShouldRender()) return;
    if(!preparedKnown) {prepared=PrepareMaterial();preparedKnown=true;}
    const auto& raster=GetNativeDrawState().raster;
    if(depthBeforeTexture && raster.depth_test && raster.depth_write && bool(prepared.config.alphaCompare))
        throw std::runtime_error("Native early depth write with alpha discard is unsupported");
    uint8_t zeroBias=0;
    for(unsigned i=0;i<8;++i) if(prepared.info.sampledTextures.test(i)) {
        (void)Texture(i);
        if(aurora::native_texture_shader_lod_bias(i)==0) zeroBias|=1u<<i;
    }
    ProgramKey key{prepared.config,zeroBias};auto iterator=programs.find(key);
    if(iterator==programs.end()) {
        // Always use the user-selected float native equations. The generator's
        // exact mode exists only for independent reference probes.
        auto description=aurora::gx::build_shader_description(prepared.config,true,zeroBias,true);
        description.origin="runtime/src/native_material.cpp:SubmitNativeMaterialGeometry";
        auto program=std::make_shared<aurora::gl::Program>(std::move(description));
        iterator=programs.emplace(std::move(key),std::move(program)).first;
    }
    AuroraNativeUniforms uniforms{};
    if(!aurora_allocate_native_uniforms(prepared.info.uniformSize,&uniforms)) return;
    WriteUniforms(prepared,uniforms);
    aurora_submit_native_material_geometry(&geometry,iterator->second->get(),&uniforms);
}
void SubmitNativeModelMesh(const mh3_resolved_mesh_submit& mesh) {
    Owner();if(!mesh.index_count) return;
    ObserveNativeModelAssetDemand(mesh.resident_handle,mesh.resident_generation);
    if(!NativeGraphicsFrameShouldRender()) return;
    if(!EnsureNativeModelAssetGPU(mesh.resident_handle,mesh.resident_generation)) {
        CancelNativeGraphicsFrame();return;
    }
    const auto resident=ResolveNativeModelGPU(mesh);
    if(!mesh.material) throw std::runtime_error("Model draw has no native material producer");
    if(mesh.material!=&material) SetNativeMaterial(*mesh.material);
    if(!preparedKnown) {prepared=PrepareMaterial();preparedKnown=true;}
    auto modelState=prepared;
    auto draw=mesh;draw.mesh=*resident.mesh;draw.material=&material;
    if(((draw.layout.flags>>MH3_LAYOUT_MATRIX_MASK_SHIFT)&0x1ffu)!=resident.layout->matrixIndexMask)
        throw std::runtime_error("Native model matrix selector layout mismatch");
    mh3_model_gpu_inputs inputs{};
    Check(mh3_model_gpu_prepare(&draw,uint8_t(modelState.info.sampledTexCoords.to_ulong()),
          modelState.info.lightingEnabled,GetNativeDrawState().viewport[4],GetNativeDrawState().viewport[5],&inputs),
          "Native model GPU inputs");
    auto& config=modelState.config;
    config.attrs={};config.vtxStride=0;
    for(const auto& a:resident.layout->attributes) {
        const unsigned location=a.location;
        if(location>=12) continue;
        const GXAttr attr=location==0?GX_VA_POS:location==1?GX_VA_NRM:
            location<4?GXAttr(GX_VA_CLR0+location-2):GXAttr(GX_VA_TEX0+location-4);
        config.attrs[attr]={GX_DIRECT,uint8_t(location<2?3:location<4?4:2),uint8_t(location>=2&&location<4?GX_RGBA8:GX_F32)};
    }
    if(inputs.matrix_index_mask&1) config.attrs[GX_VA_PNMTXIDX]={GX_DIRECT,1,GX_U8};
    for(unsigned i=0;i<8;++i) if(inputs.matrix_index_mask&(2u<<i))
        config.attrs[GX_VA_TEX0MTXIDX+i]={GX_DIRECT,1,GX_U8};
    const bool absoluteMatrices=(inputs.matrix_index_mask&1) ||
        ((inputs.matrix_index_mask>>1)&inputs.texcoord_mask);
    config.dualTexEnabled=false;
    constexpr GXTexGenSrc source[]{GX_TG_POS,GX_TG_NRM,GX_TG_COLOR0,GX_TG_BINRM,GX_TG_TANGENT,
        GX_TG_TEX0,GX_TG_TEX1,GX_TG_TEX2,GX_TG_TEX3,GX_TG_TEX4,GX_TG_TEX5,GX_TG_TEX6,GX_TG_TEX7};
    for(unsigned i=0;i<8;++i) if(inputs.texcoord_mask&(1u<<i)) {
        const auto& recipe=inputs.texgen[i];auto& gen=config.tcgs[i];
        gen.src=source[recipe.source_row];gen.inputFormAB11=recipe.input_ab11;
        gen.type=recipe.projection?GX_TG_MTX3x4:GX_TG_MTX2x4;
        gen.mtx=recipe.main_identity?GX_IDENTITY:GXTexMtx(absoluteMatrices?3*recipe.main_slot:30+3*i);
        gen.postMtx=recipe.post_identity?GX_PTIDENTITY:GXPTTexMtx(64+3*i);
        gen.normalize=recipe.normalize;
        if(gen.normalize || !recipe.post_identity) config.dualTexEnabled=true;
    }
    modelState.info=aurora::gx::build_shader_info(config);
    const auto& raster=GetNativeDrawState().raster;
    if(depthBeforeTexture && raster.depth_test && raster.depth_write && bool(config.alphaCompare))
        throw std::runtime_error("Native early depth write with alpha discard is unsupported");
    uint8_t zeroBias=0;
    for(unsigned i=0;i<8;++i) if(modelState.info.sampledTextures.test(i)) {
        (void)Texture(i);if(aurora::native_texture_shader_lod_bias(i)==0) zeroBias|=1u<<i;
    }
    ModelProgramKey key{{config,zeroBias},{},inputs.nonzero_projective_mask,inputs.zero_projective_mask};
    key.layout[0]=resident.layout->stride;
    key.layout[1]=resident.layout->matrixIndexMask;
    for(unsigned i=0;i<resident.layout->attributes.size();++i) {
        const auto& a=resident.layout->attributes[i];
        key.layout[i+2]=a.offset|(uint32_t(a.location)<<16)|(uint32_t(a.components)<<20)|(uint32_t(a.format)<<24);
    }
    auto program=modelPrograms.find(key);
    if(program==modelPrograms.end()) {
        auto description=aurora::gx::build_shader_description(config,true,zeroBias,true,resident.layout,inputs.nonzero_projective_mask,inputs.zero_projective_mask);
        description.origin="runtime/src/native_material.cpp:SubmitNativeModelMesh asset="+std::to_string(mesh.asset_id)+
            " model="+std::to_string(mesh.model_id)+" shape="+std::to_string(mesh.shape_id);
        program=modelPrograms.emplace(key,std::make_shared<aurora::gl::Program>(std::move(description))).first;
    }
    AuroraNativeUniforms uniforms{};
    if(!aurora_allocate_native_uniforms(modelState.info.uniformSize,&uniforms)) return;
    WriteUniforms(modelState,uniforms,&inputs,mesh.texcoord_sizes);
    const auto programID=program->second->get();
    aurora::gl::draw_native_mesh(*resident.buffer,*resident.layout,resident.mesh->vertex_offset,
        resident.mesh->vertex_count,resident.mesh->index_offset+mesh.first_index*2,mesh.index_count,
        programID,uniforms);
#ifdef MH3_DEVELOPMENT
    if(aurora::gfx::port_capture::enabled())
        aurora::gfx::port_capture::native_mesh_source({aurora::gfx::current_frame(),programID,
            mesh.asset_id,mesh.model_id,mesh.shape_id,mesh.mesh_index,mesh.first_index,mesh.index_count,
            mesh.resident_generation,mesh.source_generation,mesh.pose_generation,material.generation,
            mesh.layout.flags});
#endif
}
void ClearNativeMaterialPrograms() {
    Owner();programs.clear();modelPrograms.clear();ClearNativeModelGPU();preparedKnown=false;
    if(rasterObserverInstalled) SetNativeRasterObserver(nullptr);
    rasterObserverInstalled=false;
}
