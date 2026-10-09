// ROM-free GPU feedback probe. Pixel reads occur only here as diagnostics.
#include <aurora/aurora.h>
#include <aurora/native_geometry.h>
#include <aurora/native_scene_feedback.hpp>
#include <aurora/generated_texture.hpp>
#include <aurora/gpu_completion.hpp>
#include "gl/device.hpp"
#include "gl/gl.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool value,const char *reason){if(!value)throw std::runtime_error(reason);}
void pixel(std::array<uint8_t,4> expected){
  uint8_t actual[4];aurora::gl::read_pixels(aurora::gpu::g_frameBuffer.texture->gl(),32,32,1,1,actual);

  for(unsigned i=0;i<4;i++)if(std::abs(int(actual[i])-int(expected[i]))>1){std::fprintf(stderr,"Pixel: %u,%u,%u,%u expected %u,%u,%u,%u\n",actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);throw std::runtime_error("Native scene feedback pixel differs");}
}

void clear(float r,float g,float b){const float color[4]={r,g,b,1};aurora::gl::clear_color(color);}
void quad(){
  AuroraNativeGeometry geometry{};require(aurora_allocate_native_geometry(4,6,&geometry),"feedback probe geometry allocation failed");
  const float position[4][2]={{-1,1},{1,1},{1,-1},{-1,-1}},uv[4][2]={{0,0},{1,0},{1,1},{0,1}};

  for(unsigned i=0;i<4;i++){auto &v=geometry.vertices[i];v.position[0]=position[i][0];v.position[1]=position[i][1];v.position[2]=0;v.position[3]=1;std::memcpy(v.uv,uv[i],8);std::memset(v.color,255,4);}
  const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(geometry.indices,indices,12);aurora_submit_native_geometry(&geometry,true);
}

const uint8_t* composition(){
  aurora::MaterialComposition material;for(auto &source:material.sources){source.width=source.height=128;source.format=6;source.bytes.resize(128*128*4);}
  // Wii RGBA8 layout: per4x4 tile, 32 AR bytes then32 GB bytes.
  for(size_t tile=0;tile<material.sources[0].bytes.size();tile+=64)for(unsigned i=0;i<16;i++){
    material.sources[0].bytes[tile+2*i]=255;material.sources[0].bytes[tile+2*i+1]=100;
    material.sources[1].bytes[tile+2*i]=128;material.sources[1].bytes[tile+2*i+1]=200;
  }

  material.opacity=.5f;static uint8_t identity;
  aurora::publish_generated_material(123,&identity,std::move(material));
  return &identity;
}

void bind_published(const uint8_t* identity,uint32_t size,uint32_t format){
  aurora::NativeTextureSource source;source.bytes={identity,1};
  source.width=source.height=size;source.format=format;source.mutable_source=true;

  // One byte cannot be converted into this image. Binding must use the
  // explicit GPU producer's target revision, with CPU generation still zero.
  aurora::bind_native_texture(0,source,{});
}

void reject_unpublished(){
  static const uint8_t identity=0;
  bool rejected=false;

  try{bind_published(&identity,32,4);}
  catch(const std::runtime_error& error){rejected=std::strcmp(error.what(),"Mutable native texture has no producer revision")==0;}
  require(rejected,"unpublished mutable CPU source was admitted");
}

void bind_target(){
  const auto &target=aurora::gpu::g_frameBuffer;
  aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());
  aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor_enabled(false);aurora::gl::apply_raster_state({});
}

void pool_bounds(const aurora::SceneFeedbackStats& stats){
  require(stats.pool_bytes<=stats.byte_limit&&stats.pool_targets<=stats.target_limit,"feedback pool exceeded its bound");
}

void print_stats(const char* phase){
  const auto stats=aurora::scene_feedback_stats();pool_bounds(stats);
  std::printf("Feedback %s: allocations=%llu reuses=%llu evictions=%llu cancellations=%llu targets=%u bytes=%llu last=%u/%u/%u/%llu\n",phase,
    static_cast<unsigned long long>(stats.allocations),static_cast<unsigned long long>(stats.reuses),
    static_cast<unsigned long long>(stats.evictions),static_cast<unsigned long long>(stats.capacity_cancellations),stats.pool_targets,
    static_cast<unsigned long long>(stats.pool_bytes),stats.last_call_targets,stats.last_call_allocations,stats.last_call_reuses,
    static_cast<unsigned long long>(stats.last_call_bytes));
  for(uint32_t i=0;i<stats.size_count;i++){const auto& size=stats.sizes[i];
    std::printf("Feedback size %ux%u allocations=%llu reuses=%llu targets=%u bytes=%llu\n",size.width,size.height,
      static_cast<unsigned long long>(size.allocations),static_cast<unsigned long long>(size.reuses),size.pool_targets,
      static_cast<unsigned long long>(size.pool_bytes));
  }
}

void latest_notification(){
  volatile unsigned completion=4;aurora::GpuFrameRing ring;ring.record_scene(&completion,4);

  for(unsigned i=0;i<ring.BufferCount;i++)ring.advance();
  require(!ring.pending(),"completed original frame unexpectedly pending");

  // An old image sampled again in a later scene must use that later fence,
  // even after its creation slot became available. GXM's texture query uses
  // this exact helper with TextureStorage's most recent note_gpu_use value.
  require(aurora::gpu_notification_pending(&completion,7),"later image consumer was ignored");
  completion=7;require(!aurora::gpu_notification_pending(&completion,7),"completed later image stayed busy");
  completion=0xffffffffu;require(aurora::gpu_notification_pending(&completion,0),"notification wrap admitted busy image");
  completion=0;require(!aurora::gpu_notification_pending(&completion,0),"notification wrap failed to retire image");
}

void binding_gate(){
  const auto saved=aurora::gl::save_offscreen_state();
  auto probe=aurora::gl::create_texture(8,8,1,aurora::gl::Format::RGBA8,"Feedback idle binding probe");
  aurora::gl::bind_texture(0,0,0);aurora::gl::bind_texture(15,probe->gl(),0);
  require(!aurora::gl::render_target_reusable(*probe),"bound sampled target was reusable");
  aurora::gl::bind_texture(15,0,0);
  require(aurora::gl::render_target_reusable(*probe),"unbound unused target was busy");
  aurora::gl::bind_framebuffer(probe->gl(),0);
  require(!aurora::gl::render_target_reusable(*probe),"bound attachment target was reusable");
  aurora::gl::restore_offscreen_state(saved);
  const auto after=aurora::gl::save_offscreen_state();
  require(saved.textures==after.textures&&saved.samplers==after.samplers&&saved.framebuffer==after.framebuffer,"idle query changed native bindings");
}

void normal_dimensions(){
  static uint8_t snapshot,temporal,glare,saved;
  const aurora::SceneCaptureParameters capture{{0,0,64,64},640,448,6,false};
  const aurora::SceneCaptureParameters history{{0,0,64,64},320,224,4,true};
  aurora::SceneGlareParameters bloom;bloom.levels={{64,64,320,224},{320,224,160,112},{160,112,80,56}};bloom.width=80;bloom.height=56;
  aurora::NativeTextureSampler sampling;uint64_t steady_allocations=0;
  const auto before=aurora::scene_feedback_stats();

  for(unsigned epoch=0;epoch<64;epoch++){
    aurora_update();require(aurora_begin_native_frame(),"normal-size feedback frame rejected");bind_target();

    if(epoch){require(aurora::bind_scene_feedback_texture(0,&temporal,320,224,sampling),"N-1 history disappeared");quad();if(epoch%8==0)pixel({0,255,0,255});}
    clear(float(!(epoch&1)),float(epoch&1),0);require(aurora::publish_scene_capture(&snapshot,capture),"normal-size snapshot failed");

    if(!epoch)require(aurora::clone_scene_feedback_texture(&saved,&snapshot),"normal-size saved clone failed");
    clear(0,float(epoch&1),float(!(epoch&1)));require(aurora::publish_scene_capture(&temporal,history),"normal-size temporal capture failed");
    require(aurora::publish_scene_glare(&glare,bloom),"normal-size glare failed");
    const auto stats=aurora::scene_feedback_stats();pool_bounds(stats);
    require(stats.last_call_targets==4&&stats.last_call_bytes==394240,"normal-size glare reservation dimensions differ");

    if(epoch==8)steady_allocations=stats.allocations;

    if(epoch>8)require(stats.allocations==steady_allocations,"normal-size pool kept allocating after warmup");
    require(aurora::bind_scene_feedback_texture(0,&saved,640,448,sampling),"long-retained saved snapshot disappeared");quad();

    if(epoch%8==3)pixel({255,0,0,255});
    require(aurora::bind_scene_feedback_texture(0,&glare,80,56,sampling),"normal-size glare binding failed");quad();

    if(epoch%8==0)pixel({29,29,29,255});
    aurora_end_native_frame();
  }

  const auto after=aurora::scene_feedback_stats();
  require(after.capacity_cancellations==before.capacity_cancellations&&after.reuses>before.reuses,"normal-size pool did not reuse without cancellations");
  print_stats("normal64");
}

void capacity(){
  aurora::clear_native_textures();aurora::clear_scene_feedback_textures();
  const auto cleared=aurora::scene_feedback_stats();require(!cleared.pool_bytes&&!cleared.pool_targets,"clear retained feedback targets");
  static std::array<uint8_t,33> identities;
  aurora_update();require(aurora_begin_native_frame(),"capacity fixture frame rejected");bind_target();clear(0,1,0);
  const aurora::SceneCaptureParameters small{{0,0,64,64},8,8,6,false};

  for(unsigned i=0;i<31;i++)require(aurora::publish_scene_capture(&identities[i],small),"retained count fixture prematurely rejected");
  const auto first=aurora::scene_feedback_texture_version(&identities[0],8,8);
  aurora::SceneGlareParameters bloom;bloom.width=bloom.height=8;
  const auto before=aurora::scene_feedback_stats();

  // This reserves one fresh destination, then fails to reserve the second.
  // No glare pass/revision is published; the staged reference is released.
  require(!aurora::publish_scene_glare(&identities[31],bloom),"oversubscribed complete chain was admitted");
  require(!aurora_native_frame_should_render(),"capacity did not cancel presentation");
  require(first==aurora::scene_feedback_texture_version(&identities[0],8,8),"capacity discarded an earlier successful publication");
  require(!aurora::scene_feedback_texture_version(&identities[31],8,8),"failed chain published a partial revision");
  const auto after=aurora::scene_feedback_stats();pool_bounds(after);
  require(after.capacity_cancellations==before.capacity_cancellations+1&&after.pool_targets==32&&after.retained_targets==31&&after.available_targets==1,"staged reservation leaked or evicted a retained image");
  require(!aurora::publish_scene_capture(&identities[32],small),"canceled frame allocated more targets");
  const auto skipped=aurora::scene_feedback_stats();require(skipped.allocations==after.allocations&&skipped.reuses==after.reuses,"skipped frame changed pool activity");
  aurora_end_native_frame();pixel({0,255,0,255});print_stats("count-pressure");
  aurora_update();require(aurora_begin_native_frame(),"post-pressure eviction frame rejected");bind_target();clear(0,1,0);
  const aurora::SceneCaptureParameters odd{{0,0,64,64},9,7,6,false};
  require(aurora::publish_scene_capture(&identities[31],odd),"idle target was not evicted for a new descriptor");
  const auto evicted=aurora::scene_feedback_stats();
  require(evicted.evictions==after.evictions+1&&evicted.pool_targets==32&&evicted.pool_bytes==31*256+448,"idle eviction or aligned descriptor accounting differs");
  aurora::NativeTextureSampler sampling;require(aurora::bind_scene_feedback_texture(0,&identities[0],8,8,sampling),"eviction removed a retained image");quad();pixel({0,255,0,255});
  aurora_end_native_frame();
  aurora::clear_scene_feedback_textures();
  aurora_update();require(aurora_begin_native_frame(),"byte capacity frame rejected");bind_target();clear(0,0,1);
  const aurora::SceneCaptureParameters large{{0,0,64,64},1024,1024,6,false};
  require(aurora::publish_scene_capture(&identities[0],large)&&aurora::publish_scene_capture(&identities[1],large),"exact byte cap rejected fitting targets");
  const auto version=aurora::scene_feedback_texture_version(&identities[0],1024,1024);
  require(!aurora::publish_scene_capture(&identities[0],large),"byte capacity overwrote a retained revision");
  require(!aurora_native_frame_should_render()&&version==aurora::scene_feedback_texture_version(&identities[0],1024,1024),"byte capacity changed the failed producer revision");
  const auto full=aurora::scene_feedback_stats();require(full.pool_bytes==full.byte_limit&&full.pool_targets==2,"byte cap used logical rather than backing bytes");
  aurora_end_native_frame();print_stats("byte-pressure");aurora::clear_scene_feedback_textures();
}
}

int main(int argc,char **argv){
  try {
    latest_notification();
    const std::string cache=argc>1?argv[1]:"/tmp/mh3-native-scene-feedback";std::filesystem::create_directories(cache);
    require(aurora_configure_native_target(64,64),"feedback target configuration failed");AuroraConfig config{};config.appName="MH3 native scene feedback check";config.userPath=config.cachePath=cache.c_str();config.desiredBackend=BACKEND_OPENGLES;config.windowWidth=config.windowHeight=64;config.msaa=1;
    require(aurora_initialize(argc,argv,&config).initializationStatus==AURORA_INITIALIZATION_SUCCESS,"feedback renderer initialization failed");
    struct Shutdown {~Shutdown(){aurora::clear_native_textures();aurora::clear_scene_feedback_textures();aurora::clear_generated_textures();aurora_shutdown();}} shutdown;
    aurora_set_frameskip(true);static uint8_t history,glare,clone;unsigned rendered=0;
    const uint8_t* generated=nullptr;

    for(unsigned frame=0;frame<18;frame++){
      aurora_update();const bool draw=aurora_begin_native_frame();if(draw){
        rendered++;const auto &target=aurora::gpu::g_frameBuffer;aurora::gl::bind_framebuffer(target.texture->gl(),aurora::gpu::g_depthBuffer.texture->gl());aurora::gl::set_viewport(0,0,64,64,0,1);aurora::gl::set_scissor(2,3,60,59);aurora::gl::set_scissor_enabled(false);aurora::gl::apply_raster_state({});

        if(!frame)binding_gate();clear(1,0,0);const aurora::SceneCaptureParameters capture{{0,0,64,64},32,32,4,true};require(aurora::publish_scene_capture(&history,capture),"history capture not published");const auto oldRevision=aurora::scene_feedback_texture_version(&history,32,32);aurora::NativeTextureSampler sampling;sampling.min_filter=sampling.mag_filter=1;require(aurora::bind_scene_feedback_texture(0,&history,32,32,sampling),"history texture not bound");
        reject_unpublished();bind_published(&history,32,4);quad();pixel({255,0,0,255});
        clear(0,float(frame&1),float(!(frame&1)));require(aurora::publish_scene_capture(&history,capture),"new history capture not published");require(aurora::scene_feedback_texture_version(&history,32,32)!=oldRevision,"feedback revision did not advance");

        // The old published image remains bound across replacement and passes.
        quad();pixel({255,0,0,255});require(aurora::bind_scene_feedback_texture(0,&history,32,32,sampling),"new history not rebound");quad();pixel(frame&1?std::array<uint8_t,4>{0,255,0,255}:std::array<uint8_t,4>{0,0,255,255});

        if(!frame)require(aurora::clone_scene_feedback_texture(&clone,&history),"saved snapshot clone failed");clear(1,0,0);
        aurora::SceneGlareParameters bloom;bloom.levels={{64,64,32,32},{32,32,16,16}};bloom.width=bloom.height=frame&1?12:16;bloom.low_threshold=64.f/255;bloom.high_threshold=192.f/255;bloom.darken=bloom.brighten=.5f;bloom.shift_scale=.5f;require(aurora::publish_scene_glare(&glare,bloom),"glare texture not published");require(aurora::bind_scene_feedback_texture(0,&glare,bloom.width,bloom.height,sampling),"glare texture not bound");quad();pixel({38,38,38,255});
        const auto saved=aurora::gl::save_offscreen_state();generated=composition();const auto after=aurora::gl::save_offscreen_state();require(saved.framebuffer==after.framebuffer&&saved.viewport==after.viewport&&saved.scissor==after.scissor&&saved.scissorEnabled==after.scissorEnabled&&saved.textures==after.textures&&saved.samplers==after.samplers&&saved.program==after.program&&saved.vertexArray==after.vertexArray,"generated composition leaked native state");
        bind_published(generated,128,6);quad();pixel({125,0,0,255});
        require(aurora::bind_scene_feedback_texture(0,&clone,32,32,sampling),"saved clone not bound");quad();pixel({0,0,255,255});
      }aurora_end_native_frame();
    }

    require(rendered==18,"feedback probe skipped host frames");print_stats("retention18");
    require(aurora_begin_native_frame(),"revision probe frame admission failed");
    const auto saved=aurora::gl::save_offscreen_state();
    const auto generatedRevision=aurora::generated_texture_version(generated,128,128);
    const auto historyRevision=aurora::scene_feedback_texture_version(&history,32,32);
    aurora_cancel_native_frame();
    require(!aurora_native_frame_should_render(),"revision probe did not suppress rendering");
    reject_unpublished();bind_published(generated,128,6);bind_published(&history,32,4);
    const auto after=aurora::gl::save_offscreen_state();
    require(saved.textures==after.textures&&saved.samplers==after.samplers,"suppressed binding changed GPU texture state");
    require(generatedRevision==aurora::generated_texture_version(generated,128,128)&&historyRevision==aurora::scene_feedback_texture_version(&history,32,32),"suppressed binding changed GPU publication revisions");
    require(glGetError()==GL_NO_ERROR,"revision probe emitted a GLES error");aurora_end_native_frame();
    normal_dimensions();capacity();
    std::printf("Native scene feedback passed: %u retention frames + 64 normal-size epochs, latest-use notifications, bounded reuse, staged rejection, GPU revisions with no CPU image bytes, suppressed bindings, saved clones, glare and state restoration\n",rendered);return 0;
  } catch(const std::exception &e){std::fprintf(stderr,"Native scene feedback failed: %s\n",e.what());return 1;}
}
