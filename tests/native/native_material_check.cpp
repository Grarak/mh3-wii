// ROM-free GLES probe of the real typed runtime material mapper.
#include <aurora/aurora.h>
#include <aurora/native_texture.hpp>
#include "native_graphics.h"
#include "../../runtime/native/mh3_material_state.h"
#include "../../runtime/native/mh3_model_gpu_inputs.h"
#include "../../runtime/native/adapters/model_gpu_bridge.h"
#include <fstream>
#include <vector>
#include "gl/gl.hpp"
#include "gl/device.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

static bool suppressed=false;
static mh3_native_texture_binding textureBinding{};
static bool texturePublished=false;
bool NativeGraphicsFrameShouldRender() {return !suppressed && aurora_native_frame_should_render();}
// The ROM-free resident fixture registers its immutable GPU buffer directly.
bool EnsureNativeModelAssetGPU(uintptr_t,uint64_t) {return true;}
void ObserveNativeModelAssetDemand(uintptr_t,uint64_t) {}
void PublishNativeModelTextureMatrix(uint32_t,const float[12],bool) {}
void PublishNativeModelPositionMatrix(uint32_t,const float[12]) {}
void PublishNativeModelNormalMatrix(uint32_t,const float[9]) {}
void CancelNativeGraphicsFrame() {aurora_cancel_native_frame();}
const mh3_native_texture_binding* GetNativeTextureBinding(uint32_t unit) {
    return unit==0 && texturePublished ? &textureBinding : nullptr;
}
namespace {
constexpr uint64_t Bit(unsigned field) {return UINT64_C(1)<<field;}
void Require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
template<typename F> void Failure(F&& operation,const char* fragment) {
    try {operation();} catch(const std::runtime_error& error) {
        Require(std::string(error.what()).find(fragment)!=std::string::npos,"unexpected mapper failure");return;
    }
    throw std::runtime_error("required mapper failure was not reported");
}
void Pixel(unsigned x,unsigned y,const std::array<uint8_t,4>& expected) {
    uint8_t actual[4]{};
    const auto& target=aurora::gpu::g_frameBuffer;
    const unsigned pixelX=unsigned((float(x)+.5f)*target.width/64);
    const unsigned pixelY=unsigned((63.5f-float(y))*target.height/64);
    aurora::gl::read_pixels(target.texture->gl(),pixelX,pixelY,1,1,actual);
    for(unsigned i=0;i<4;++i) if(std::abs(int(actual[i])-int(expected[i]))>1) {
        std::fprintf(stderr,"material pixel %u,%u: %u,%u,%u,%u expected %u,%u,%u,%u\n",x,y,
            actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
        throw std::runtime_error("typed runtime material pixel mismatch");
    }
}
AuroraNativeMaterialGeometry Quad(float left,float right,float bottom,float top,
                                  const std::array<uint8_t,4>& color={255,255,255,255}) {
    AuroraNativeMaterialGeometry geometry{};
    Require(aurora_allocate_native_material_geometry(4,6,&geometry),"material geometry allocation failed");
    std::memset(geometry.vertices,0,4*sizeof(*geometry.vertices));
    const float positions[][2]={{left,bottom},{right,bottom},{right,top},{left,top}};
    for(unsigned i=0;i<4;++i) {
        auto& vertex=geometry.vertices[i];vertex.position[0]=positions[i][0];vertex.position[1]=positions[i][1];
        vertex.position[2]=.8f;vertex.position[3]=1;vertex.normal[2]=1;
        for(unsigned map=0;map<8;++map) {vertex.texcoord[map][0]=.5f;vertex.texcoord[map][1]=.5f;vertex.texcoord[map][2]=2;}
        std::memcpy(vertex.color[0],color.data(),4);std::memcpy(vertex.color[1],color.data(),4);
    }
    const uint16_t indices[]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,sizeof indices);return geometry;
}
mh3_native_material_state Plain() {
    mh3_native_material_state state{};state.tev_count=1;state.light_set_reference=state.fog_reference=-1;
    state.tev[0].known=(UINT64_C(1)<<18)-1;
    state.tev[0].color={15,15,15,12,0,0,0,1,0};state.tev[0].alpha={7,7,7,6,0,0,0,1,0};
    state.tev[0].known|=Bit(MH3_TEV_KONST_ALPHA);state.tev[0].konst_alpha=0;
    state.pixel.known=Bit(MH3_PIXEL_ALPHA_COMPARE0)|Bit(MH3_PIXEL_ALPHA_COMPARE1)|Bit(MH3_PIXEL_ALPHA_OPERATION);
    state.pixel.values[MH3_PIXEL_ALPHA_COMPARE0]=state.pixel.values[MH3_PIXEL_ALPHA_COMPARE1]=7;
    return state;
}
void Texture() {
    std::array<uint8_t,64> tiled{};
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
        const unsigned i=y*4+x;
        tiled[2*i]=255;tiled[2*i+1]=x<2?128:0;tiled[32+2*i]=x<2?0:255;
    }
    aurora::NativeTextureSource source;source.bytes=tiled;source.width=source.height=4;source.levels=1;source.format=6;source.generation=1;
    aurora::bind_native_texture(0,source,{});
    textureBinding.source.width=textureBinding.source.height=4;textureBinding.source.levels=1;textureBinding.source.format=6;
    textureBinding.source.generation=1;texturePublished=true;
}
void ResidentProbe(const char* path) {
    std::ifstream file(path,std::ios::binary);Require(bool(file),"model fixture open failed");
    std::array<uint8_t,20> header{};file.read(reinterpret_cast<char*>(header.data()),header.size());
    const unsigned bytes=unsigned(header[8])|(unsigned(header[9])<<8)|(unsigned(header[10])<<16)|(unsigned(header[11])<<24);
    Require(bool(file)&&bytes>=128&&bytes<16*1024*1024,"model metadata size invalid");
    std::vector<uint8_t> metadata(bytes);file.seekg(0);file.read(reinterpret_cast<char*>(metadata.data()),bytes);
    Require(bool(file),"model metadata truncated");mh3_mesh_pack pack{};
    Require(mh3_mesh_pack_open_metadata({metadata.data(),metadata.size()},&pack)==MH3_PACK_OK,"model metadata invalid");
    RegisterNativeModelGPU(pack,path,1,1);
    struct Retire {~Retire(){UnregisterNativeModelGPU(1,1);}} retire;
    mh3_resolved_mesh_submit draw{};draw.pack=&pack;draw.resident_handle=1;
    draw.resident_generation=draw.source_generation=draw.pose_generation=1;
    bool found=false;
    for(unsigned i=0;i<pack.counts[MH3_PACK_MESHES];++i) {
        Require(mh3_mesh_pack_mesh(&pack,i,&draw.mesh)==MH3_PACK_OK,"fixture mesh invalid");
        if(draw.mesh.matrix_id>=0 && draw.mesh.index_count && draw.mesh.bounds_max[0]-draw.mesh.bounds_min[0]>1 &&
           draw.mesh.bounds_max[2]-draw.mesh.bounds_min[2]>1) {draw.mesh_index=i;found=true;break;}
    }
    Require(found,"no rigid mesh fixture with visible XZ extent");
    Require(mh3_mesh_pack_layout(&pack,draw.mesh.layout_index,&draw.layout)==MH3_PACK_OK,"fixture layout invalid");
    draw.asset_id=draw.mesh.asset_id;draw.model_id=draw.mesh.model_id;draw.shape_id=draw.mesh.source_id;
    draw.index_count=draw.mesh.index_count;
    mh3_model_palette palette{};palette.position_valid=1;
    const float sx=1.6f/(draw.mesh.bounds_max[0]-draw.mesh.bounds_min[0]);
    const float sy=1.6f/(draw.mesh.bounds_max[2]-draw.mesh.bounds_min[2]);
    palette.position[0].v[0][0]=sx;palette.position[0].v[0][3]=-.5f*(draw.mesh.bounds_min[0]+draw.mesh.bounds_max[0])*sx;
    palette.position[0].v[1][2]=sy;palette.position[0].v[1][3]=-.5f*(draw.mesh.bounds_min[2]+draw.mesh.bounds_max[2])*sy;
    palette.position[0].v[2][3]=-.5f;draw.palette=&palette;
    const float projection[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};draw.projection=projection;
    auto state=Plain();draw.material=&state;
    SetNativeViewport(0,0,64,64,0,1);SetNativeScissor(0,0,64,64);
    SetNativeRaster({0,1,0,3,0,7,0,0,1,1});
    SubmitNativeModelMesh(draw);
    const auto& target=aurora::gpu::g_frameBuffer;
    std::vector<uint8_t> pixels(size_t(target.width)*target.height*4);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,pixels.data());
    unsigned white=0;
    for(size_t i=0;i<pixels.size();i+=4) if(pixels[i]>250&&pixels[i+1]>250&&pixels[i+2]>250) ++white;
    Require(white>100,"resident runtime model produced no useful coverage");
    // A direct exactly-zero Q row and a composed identity-post recipe have
    // identical vertex equations. Only the direct recipe is certified. Compare
    // the complete texture image, including the existing zero-Q XY clamp.
    Texture();state=Plain();state.texgen_count=1;
    state.tev[0].known=MH3_MATERIAL_TEV_MASK;
    state.tev[0].color={15,15,15,8,0,0,0,1,0};state.tev[0].alpha={7,7,7,4,0,0,0,1,0};
    state.swaps[0]={15,{0,1,2,3}};
    state.texgen[0].known=(1u<<MH3_TEXGEN_FIELD_COUNT)-1;
    auto& texgen=state.texgen[0];texgen.values[MH3_TEXGEN_PROJECTION]=1;
    texgen.values[MH3_TEXGEN_SOURCE_ROW]=0;texgen.values[MH3_TEXGEN_MATRIX_ROW]=30;
    texgen.values[MH3_TEXGEN_POST_MATRIX_ROW]=61;
    palette.texture_valid=1;palette.texture[0]={{{0,0,0,1.5f},{0,0,0,1},{0,0,0,0}}};
    mh3_model_gpu_inputs inputs{};
    Require(mh3_model_gpu_prepare(&draw,1,0,0,1,&inputs)==MH3_RES_OK && inputs.zero_projective_mask==1,
        "resident direct zero-Q certificate missing");
    const float clear[]{0,0,0,1};aurora::gl::clear_color(clear);
    SubmitNativeModelMesh(draw);
    std::vector<uint8_t> certified(pixels.size());
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,certified.data());
    unsigned green=0;
    for(size_t i=0;i<certified.size();i+=4) if(certified[i]<2 && certified[i+1]>250 && certified[i+2]<2) ++green;
    Require(green==white,"certified zero-Q changed resident texture/coverage");
    float identityPost[8][12]={{1,0,0,0,0,1,0,0,0,0,1,0}};
    draw.texture_matrices=identityPost;draw.texture_matrix_known=1;
    texgen.values[MH3_TEXGEN_DUAL_TEXTURE]=1;texgen.values[MH3_TEXGEN_POST_MATRIX_ROW]=0;
    Require(mh3_model_gpu_prepare(&draw,1,0,0,1,&inputs)==MH3_RES_OK && inputs.zero_projective_mask==0,
        "composed zero-Q bypassed conservative proof");
    aurora::gl::clear_color(clear);SubmitNativeModelMesh(draw);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,pixels.data());
    Require(pixels==certified,"certified and fallback zero-Q pixels differ");
    // Varying coordinates also exercise interpolation and saturation at +/-1.
    const float tx=6.f/(draw.mesh.bounds_max[0]-draw.mesh.bounds_min[0]);
    const float tz=6.f/(draw.mesh.bounds_max[2]-draw.mesh.bounds_min[2]);
    palette.texture[0]={{{tx,0,0,-.5f*(draw.mesh.bounds_min[0]+draw.mesh.bounds_max[0])*tx},
                        {0,0,tz,-.5f*(draw.mesh.bounds_min[2]+draw.mesh.bounds_max[2])*tz},
                        {0,0,0,0}}};
    texgen.values[MH3_TEXGEN_DUAL_TEXTURE]=0;texgen.values[MH3_TEXGEN_POST_MATRIX_ROW]=61;
    aurora::gl::clear_color(clear);SubmitNativeModelMesh(draw);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,certified.data());
    bool hasRed=false,hasGreen=false;
    for(size_t i=0;i<certified.size();i+=4) {
        hasRed|=certified[i]>100 && certified[i+1]<2;
        hasGreen|=certified[i]<2 && certified[i+1]>250;
    }
    Require(hasRed&&hasGreen,"varying zero-Q fixture failed to exercise texture coordinates");
    texgen.values[MH3_TEXGEN_DUAL_TEXTURE]=1;texgen.values[MH3_TEXGEN_POST_MATRIX_ROW]=0;
    aurora::gl::clear_color(clear);SubmitNativeModelMesh(draw);
    aurora::gl::read_pixels(target.texture->gl(),0,0,target.width,target.height,pixels.data());
    Require(pixels==certified,"varying/saturated zero-Q pixels differ from fallback");
    std::printf("Resident zero-Q passed: certified/fallback %u green pixels, identical constant/varying/saturated framebuffers\n",green);
    ++draw.resident_generation;
    Failure([&]{SubmitNativeModelMesh(draw);},"Stale or unregistered");
    suppressed=true;SubmitNativeModelMesh(draw);suppressed=false;
    std::printf("Resident runtime model passed: mesh %u, %u indices, %u white pixels, stale generation and frameskip\n",
                draw.mesh_index,draw.index_count,white);
}
void Probe() {
    SetNativeViewport(0,0,64,64,0,1);SetNativeScissor(0,0,64,64);
    SetNativeRaster({0,1,0,3,0,7,0,0,1,1});
    float matrix[12]{},copied[12];std::memset(copied,0xa5,sizeof copied);bool projection=true;
    Require(!GetNativeTextureMatrix(30,copied,&projection) && projection,"unknown texture matrix manufactured state");
    mh3_draw_identity_position(matrix);matrix[3]=.25f;SetNativeTextureMatrix(30,matrix,false);
    Require(GetNativeTextureMatrix(30,copied,&projection) && !projection && std::memcmp(matrix,copied,sizeof matrix)==0,
        "texture matrix producer was not retained");
    auto state=Plain();SetNativeMaterial(state);
    auto plain=Quad(-1,-.5f,.5f,1);SubmitNativeMaterialGeometry(plain);Pixel(8,56,{255,255,255,255});
    // No TEVPREV initialization is required by the preceding overwrite.
    state.tev[0].color.d=0;SetNativeMaterial(state);
    Failure([&]{SubmitNativeMaterialGeometry(plain);},"group 4");
    suppressed=true;SubmitNativeMaterialGeometry(plain);
    Require(GetNativeMaterialState().tev[0].color.d==0,"skipped material state was lost");suppressed=false;
    state=Plain();state.tev[0].color.d=14;state.tev[0].known|=Bit(MH3_TEV_KONST_COLOR);
    state.tev[0].konst_color=state.tev[0].konst_alpha=20;state.konst[0].known=2;state.konst[0].rgba[1]=192;
    SetNativeMaterial(state);SubmitNativeMaterialGeometry(Quad(-.5f,0,.5f,1));Pixel(24,56,{192,192,192,192});
    // Only alpha channel control and material alpha are consumed by this swap.
    state=Plain();state.channel_count=1;state.tev[0].color.d=11;state.tev[0].alpha.d=5;
    state.tev[0].known|=Bit(MH3_TEV_RASTER_CHANNEL)|Bit(MH3_TEV_RASTER_SWAP);state.tev[0].raster_channel=4;
    state.swaps[0].known=8;state.swaps[0].rgba[3]=3;
    state.channel[0].flags=MH3_CHANNEL_CONTROL_ALPHA|MH3_CHANNEL_MATERIAL_ALPHA;
    state.channel[0].alpha={0,0,0,0,2,0};state.channel[0].material_color[3]=64;
    SetNativeMaterial(state);SubmitNativeMaterialGeometry(Quad(0,.5f,.5f,1));Pixel(40,56,{64,64,64,64});
    Texture();state=Plain();state.texgen_count=state.channel_count=1;
    state.tev[0].known=MH3_MATERIAL_TEV_MASK;state.tev[0].color={15,8,10,15,0,0,0,1,0};state.tev[0].alpha={7,4,5,7,0,0,0,1,0};
    state.tev[0].raster_channel=4;state.swaps[0]={15,{0,1,2,3}};
    state.channel[0].flags=MH3_CHANNEL_CONTROL_COLOR|MH3_CHANNEL_CONTROL_ALPHA;
    state.channel[0].color=state.channel[0].alpha={1,0,0,0,2,0};
    state.texgen[0].known=1;state.texgen[0].values[MH3_TEXGEN_PROJECTION]=1;
    SetNativeMaterial(state);auto textured=Quad(.5f,1,.5f,1,{255,128,255,255});
    SubmitNativeMaterialGeometry(textured);Pixel(56,56,{128,0,0,255});
    // A new coordinate's projection cannot inherit an unexplained default.
    state.texgen_count=2;state.tev[0].texture_coordinate=1;SetNativeMaterial(state);
    Failure([&]{SubmitNativeMaterialGeometry(textured);},"group 2, index 1");
    state.texgen[1].known=1;state.texgen[1].values[0]=1;SetNativeMaterial(state);
    SubmitNativeMaterialGeometry(textured);Pixel(56,56,{128,0,0,255});
    state=Plain();state.pixel.known|=Bit(MH3_PIXEL_ALPHA_REFERENCE0);state.pixel.values[MH3_PIXEL_ALPHA_COMPARE0]=4;state.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0]=200;
    SetNativeMaterial(state);auto rejected=Quad(-1,-.5f,0,.5f,{255,255,255,128});
    // Plain alpha is constant1; use vertex alpha through the published channel.
    state.channel_count=1;state.tev[0].alpha.d=5;state.tev[0].known|=Bit(MH3_TEV_RASTER_CHANNEL)|Bit(MH3_TEV_RASTER_SWAP);
    state.tev[0].raster_channel=4;state.channel[0].flags=MH3_CHANNEL_CONTROL_ALPHA;state.channel[0].alpha={1,0,0,0,2,0};
    SetNativeMaterial(state);SubmitNativeMaterialGeometry(rejected);Pixel(8,40,{0,0,0,255});
    SetNativeDepthCompareBeforeTexture(true);SetNativeRaster({0,1,0,3,1,7,1,0,1,1});
    Failure([&]{SubmitNativeMaterialGeometry(rejected);},"early depth write");
    SetNativeRaster({0,1,0,3,1,7,0,0,1,1});SubmitNativeMaterialGeometry(rejected);SetNativeDepthCompareBeforeTexture(false);
    // XOR of two ALWAYS comparisons is an authored constant false alpha test.
    state=Plain();state.pixel.values[MH3_PIXEL_ALPHA_OPERATION]=2;SetNativeMaterial(state);
    SubmitNativeMaterialGeometry(Quad(-.5f,0,0,.5f));Pixel(24,40,{0,0,0,255});
    state=Plain();SetNativeMaterial(state);mh3_native_fog fog{};fog.type=10;fog.end=fog.far_plane=1;fog.color={0,0,255,255};
    SetNativeFog(fog);SubmitNativeMaterialGeometry(Quad(0,.5f,0,.5f));Pixel(40,40,{230,230,255,255});
    fog.type=0;SetNativeFog(fog);
    mh3_native_light_slots slots{};slots.valid_mask=1;slots.lights[0].color={32,64,96,255};slots.lights[0].position.v[2]=10;
    SetNativeModelLights(slots);state=Plain();state.channel_count=1;state.tev[0].color.d=10;state.tev[0].alpha.d=5;
    state.tev[0].known|=Bit(MH3_TEV_RASTER_CHANNEL)|Bit(MH3_TEV_RASTER_SWAP);state.tev[0].raster_channel=4;
    state.channel[0].flags=MH3_CHANNEL_CONTROL_COLOR|MH3_CHANNEL_CONTROL_ALPHA|MH3_CHANNEL_AMBIENT_COLOR;
    state.channel[0].color={1,0,1,0,2,1};state.channel[0].alpha={1,0,0,0,2,0};state.channel[0].ambient_color[0]=128;
    SetNativeMaterial(state);SubmitNativeMaterialGeometry(Quad(.5f,1,0,.5f));Pixel(56,40,{160,63,95,255});
    slots.valid_mask=0;SetNativeModelLights(slots);Failure([&]{SubmitNativeMaterialGeometry(textured);},"unpublished light slot");
    // Latest direct raster changes become inherited material provenance.
    SetNativeRaster({3,6,7,12,1,4,0,2,0,1});const auto& inherited=GetNativeMaterialState();
    Require(inherited.cull_mode==2 && inherited.pixel.values[MH3_PIXEL_BLEND_SUBTRACT]==1 &&
        inherited.pixel.values[MH3_PIXEL_BLEND_ENABLE]==0 && inherited.pixel.values[MH3_PIXEL_COLOR_WRITE]==0,
        "live raster provenance was not synchronized");
    // Lower-priority authored enables survive until a direct mode publication.
    state=Plain();state.pixel.known|=Bit(MH3_PIXEL_BLEND_ENABLE)|Bit(MH3_PIXEL_LOGIC_ENABLE)|Bit(MH3_PIXEL_BLEND_SUBTRACT)|Bit(MH3_PIXEL_LOGIC_OPERATION);
    state.pixel.values[MH3_PIXEL_BLEND_ENABLE]=state.pixel.values[MH3_PIXEL_LOGIC_ENABLE]=1;
    state.pixel.values[MH3_PIXEL_LOGIC_OPERATION]=3;SetNativeMaterial(state);
    Require(GetNativeMaterialState().pixel.values[MH3_PIXEL_LOGIC_ENABLE]==1,"material export collapsed authored mode bits");
    auto sameBlend=GetNativeDrawState().raster;SetNativeRaster(sameBlend);
    Require(GetNativeMaterialState().pixel.values[MH3_PIXEL_LOGIC_ENABLE]==0,"same-mode direct publication failed to clear inactive mode bits");
    SetNativeMaterial(state);
    state=GetNativeMaterialState();state.pixel.known=Bit(MH3_PIXEL_BLEND_ENABLE);state.pixel.values[MH3_PIXEL_BLEND_ENABLE]=0;
    SetNativeMaterial(state);Require(GetNativeDrawState().raster.blend_mode==2,"partial blend update lost inherited logic enable");
    // Re-publishing that same semantic mode is still a real producer action.
    auto direct=GetNativeDrawState().raster;SetNativeRaster(direct);
    Require(GetNativeMaterialState().pixel.values[MH3_PIXEL_BLEND_ENABLE]==0 &&
        GetNativeMaterialState().pixel.values[MH3_PIXEL_LOGIC_ENABLE]==1,"same-mode direct raster publication was lost");
    // Sparse publications remain CPU work during a skipped logical frame.
    // Unknown top fields inherit; invalid producers still reject atomically.
    const auto beforeSkip=GetNativeMaterialState();auto skipped=Quad(-1,1,-1,1);
    mh3_native_material_state patch{};patch.generation=UINT64_MAX;patch.tev_count=16;
    patch.pixel.known=Bit(MH3_PIXEL_ALPHA_REFERENCE0);patch.pixel.values[MH3_PIXEL_ALPHA_REFERENCE0]=91;
    suppressed=true;OverlayNativeMaterial(patch);SubmitNativeMaterialGeometry(skipped);
    Require(GetNativeMaterialState().pixel.values[MH3_PIXEL_ALPHA_REFERENCE0]==91 &&
        GetNativeMaterialState().generation==beforeSkip.generation && GetNativeMaterialState().tev_count==beforeSkip.tev_count,
        "skipped sparse material publication changed unknown top fields");
    const auto published=GetNativeMaterialState();const auto rasterBeforeFailure=GetNativeDrawState().raster;
    patch.tev[0].known=Bit(MH3_TEV_COLOR_OPERATION);patch.tev[0].color.operation=2;
    Failure([&]{OverlayNativeMaterial(patch);},"Native material overlay");
    Require(std::memcmp(&published,&GetNativeMaterialState(),sizeof published)==0 &&
        std::memcmp(&rasterBeforeFailure,&GetNativeDrawState().raster,sizeof rasterBeforeFailure)==0,
        "invalid skipped material publication changed owned state or raster");
    // Source and current may be the same owned object. The candidate must
    // finish reading it before the material/raster publication commits.
    OverlayNativeMaterial(GetNativeMaterialState(),MH3_MATERIAL_TOP_ALL);
    Require(std::memcmp(&published,&GetNativeMaterialState(),sizeof published)==0 &&
        std::memcmp(&rasterBeforeFailure,&GetNativeDrawState().raster,sizeof rasterBeforeFailure)==0,
        "self-aliased skipped publication changed material or raster bytes");
    suppressed=false;SetNativeMaterial(beforeSkip);
}
}
int main(int argc,char** argv) {
    try {
        const std::string path=argc>1?argv[1]:"/tmp/mh3-native-material-check";
        std::filesystem::create_directories(path);Require(aurora_configure_native_target(64,64),"native target setup failed");
        AuroraConfig config{};config.appName="MH3 typed material check";config.userPath=config.cachePath=path.c_str();
        config.desiredBackend=BACKEND_OPENGLES;config.windowWidth=config.windowHeight=64;config.msaa=1;
        Require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,"graphics initialization failed");
        struct Shutdown {~Shutdown(){ClearNativeMaterialPrograms();aurora_shutdown();}} shutdown;
        Require(aurora_begin_native_frame(),"native frame admission failed");
        aurora::gl::bind_framebuffer(aurora::gpu::g_frameBuffer.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
        aurora::gl::apply_raster_state({});
        const float clear[]{0,0,0,1};aurora::gl::clear_color(clear);aurora::gl::clear_depth(0);
        if(argc>2) {ResidentProbe(argv[2]);aurora::gl::clear_color(clear);}
        Probe();aurora_end_native_frame();std::puts("typed native material mapper: pixel and dependency probes passed");
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
}
