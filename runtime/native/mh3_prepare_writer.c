#include "mh3_prepare_writer.h"
#include <float.h>
#include <string.h>

_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24 && FLT_MAX_EXP==128,
               "Preparation requires IEEE binary32");
static const uint32_t mesh_sizes[8]={128,192,160,128,128,64,32,64};
static const uint32_t identity_sizes[4]={64,24,24,24};
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v) {put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static int alignment(uint32_t a) {return a && a<=4096 && !(a&(a-1));}
static uint64_t aligned(uint64_t n,uint32_t a) {return (n+a-1)&~(uint64_t)(a-1);}
uint32_t mh3_prepare_float_bits(float f) {uint32_t w;memcpy(&w,&f,4);return w;}
static int active(const mh3_prepare_writer *w,uint32_t kind) {return w && w->data && w->kind==kind && !w->sealed;}
static int complete(const mh3_prepare_writer *w) {
    for(uint32_t i=0;i<w->tables;i++)if(w->written[i]!=w->counts[i])return 0;
    return 1;
}
static mh3_prepare_status begin(mh3_prepare_writer *out,uint8_t *data,size_t capacity,
    const uint32_t *counts,uint32_t tables,uint32_t kind) {
    if(!out || !data || !counts)return MH3_PREPARE_INVALID;
    const uint32_t *sizes=kind==1?mesh_sizes:identity_sizes;
    const size_t cap=kind==1?MH3_PREPARE_MAX_METADATA:MH3_PREPARE_MAX_IDENTITY;
    uint64_t bytes=kind==1?128:64;
    mh3_prepare_writer next={0};next.data=data;next.capacity=capacity<cap?capacity:cap;
    next.tables=tables;next.kind=kind;
    for(uint32_t i=0;i<tables;i++) {
        if(bytes>UINT32_MAX)return MH3_PREPARE_RANGE;
        next.offsets[i]=(uint32_t)bytes;next.counts[i]=counts[i];
        bytes+=(uint64_t)counts[i]*sizes[i];
    }
    if(bytes>next.capacity)return MH3_PREPARE_RANGE;
    next.size=(uint32_t)bytes;memset(data,0,next.size);*out=next;return MH3_PREPARE_OK;
}
mh3_prepare_status mh3_prepare_mesh_begin(mh3_prepare_writer *out,uint8_t *data,size_t capacity,
    const uint32_t counts[8],mh3_prepare_sink gpu) {
    if(!gpu.write || !gpu.truncate || !out || !data || !counts)return MH3_PREPARE_INVALID;
    uint64_t bytes=128;for(unsigned i=0;i<8;i++)bytes+=(uint64_t)counts[i]*mesh_sizes[i];
    if(bytes>capacity || bytes>MH3_PREPARE_MAX_METADATA)return MH3_PREPARE_RANGE;
    if(gpu.truncate(gpu.context,0))return MH3_PREPARE_IO;
    mh3_prepare_status s=begin(out,data,capacity,counts,8,1);
    if(s==MH3_PREPARE_OK)out->gpu=gpu;
    return s;
}
mh3_prepare_status mh3_prepare_identity_begin(mh3_prepare_writer *out,uint8_t *data,size_t capacity,
    uint32_t models,uint32_t shapes,uint32_t materials) {
    if(!models || materials>UINT32_MAX/5)return MH3_PREPARE_RANGE;
    const uint32_t counts[4]={models,shapes,materials,materials*5};
    return begin(out,data,capacity,counts,4,2);
}
mh3_prepare_status mh3_prepare_record(mh3_prepare_writer *w,uint32_t table,
    const uint32_t *words,size_t count) {
    if(!w || !w->data || w->sealed || (w->kind!=1 && w->kind!=2) || (!words && count))return MH3_PREPARE_INVALID;
    if(table>=w->tables)return MH3_PREPARE_RANGE;
    const uint32_t bytes=(w->kind==1?mesh_sizes:identity_sizes)[table];
    if(count>bytes/4 || w->written[table]>=w->counts[table])return MH3_PREPARE_RANGE;
    uint8_t *p=w->data+w->offsets[table]+(size_t)w->written[table]*bytes;
    for(size_t i=0;i<count;i++)put32(p+i*4,words[i]);
    memset(p+count*4,0,bytes-count*4);w->written[table]++;return MH3_PREPARE_OK;
}
mh3_prepare_status mh3_prepare_blob(mh3_prepare_writer *w,const void *data,size_t bytes,
    uint32_t a,uint32_t *offset) {
    if(!active(w,1) || !offset || (!data && bytes) || !alignment(a))return MH3_PREPARE_INVALID;
    const uint64_t at=aligned(w->size,a);
    if(at>w->capacity || bytes>w->capacity-at)return MH3_PREPARE_RANGE;
    /* memmove permits references to earlier caller-owned metadata blobs. */
    if(bytes)memmove(w->data+(size_t)at,data,bytes);
    memset(w->data+w->size,0,(size_t)at-w->size);
    *offset=(uint32_t)at;w->size=(uint32_t)(at+bytes);return MH3_PREPARE_OK;
}
static int zeros(mh3_prepare_sink sink,uint64_t at,uint64_t count) {
    const uint8_t zero[64]={0};
    while(count) {size_t n=count>sizeof zero?sizeof zero:(size_t)count;
        if(sink.write(sink.context,at,zero,n))return -1;
        at+=n;count-=n;
    }
    return 0;
}
static mh3_prepare_status rollback(mh3_prepare_writer *w,uint64_t old) {
    if(w->gpu.truncate(w->gpu.context,old)) {w->sealed=2;return MH3_PREPARE_ROLLBACK;}
    return MH3_PREPARE_IO;
}
mh3_prepare_status mh3_prepare_gpu(mh3_prepare_writer *w,const void *data,size_t bytes,
    uint32_t a,uint32_t *offset) {
    if(!active(w,1) || !offset || (!data && bytes) || !alignment(a))return MH3_PREPARE_INVALID;
    const uint64_t at=aligned(w->gpu_bytes,a);
    if(at>MH3_PREPARE_MAX_GPU || bytes>MH3_PREPARE_MAX_GPU-at ||
       aligned(at+bytes+64,64)>MH3_PREPARE_MAX_GPU)return MH3_PREPARE_RANGE;
    if(zeros(w->gpu,w->gpu_bytes,at-w->gpu_bytes) ||
        (bytes && w->gpu.write(w->gpu.context,at,data,bytes)))return rollback(w,w->gpu_bytes);
    *offset=(uint32_t)at;w->gpu_bytes=(uint32_t)(at+bytes);return MH3_PREPARE_OK;
}
mh3_prepare_status mh3_prepare_mesh_finish(mh3_prepare_writer *w,mh3_mesh_pack *out) {
    if(!active(w,1) || !out || !complete(w))return MH3_PREPARE_INVALID;
    const uint64_t metadata=aligned(w->size,64),gpu=aligned((uint64_t)w->gpu_bytes+64,64);
    if(metadata>w->capacity || gpu>MH3_PREPARE_MAX_GPU)return MH3_PREPARE_RANGE;
    uint8_t previous[128];memcpy(previous,w->data,128);memset(w->data,0,128);
    memcpy(w->data,"M3MP",4);put32(w->data+4,3);put32(w->data+8,(uint32_t)metadata);
    put32(w->data+12,(uint32_t)aligned(metadata,4096));put32(w->data+16,(uint32_t)gpu);
    for(unsigned i=0;i<8;i++) {put32(w->data+20+i*8,w->counts[i]);put32(w->data+24+i*8,w->offsets[i]);}
    memset(w->data+w->size,0,(size_t)metadata-w->size);
    mh3_mesh_pack pack;
    if(mh3_mesh_pack_open_metadata((mh3_pack_span){w->data,(size_t)metadata},&pack)!=MH3_PACK_OK) {
        memcpy(w->data,previous,128);return MH3_PREPARE_INVALID;
    }
    if(zeros(w->gpu,w->gpu_bytes,gpu-w->gpu_bytes)) {
        memcpy(w->data,previous,128);return rollback(w,w->gpu_bytes);
    }
    w->size=(uint32_t)metadata;w->gpu_bytes=(uint32_t)gpu;w->sealed=1;*out=pack;return MH3_PREPARE_OK;
}
mh3_prepare_status mh3_prepare_identity_finish(mh3_prepare_writer *w,
    const mh3_mesh_pack *pack,mh3_model_catalog *out) {
    if(!active(w,2) || !pack || !out || !complete(w))return MH3_PREPARE_INVALID;
    uint8_t previous[64];memcpy(previous,w->data,64);memset(w->data,0,64);
    memcpy(w->data,"M3ID",4);put32(w->data+4,2);put32(w->data+8,w->size);
    for(unsigned i=0;i<4;i++) {put32(w->data+12+i*8,w->counts[i]);put32(w->data+16+i*8,w->offsets[i]);}
    mh3_model_catalog catalog;
    if(mh3_model_catalog_open((mh3_pack_span){w->data,w->size},&catalog)!=MH3_PACK_OK ||
       mh3_model_catalog_validate_pack(&catalog,pack)!=MH3_PACK_OK)goto invalid;
    for(uint32_t i=0;i<catalog.shape_count;i++) {
        mh3_catalog_resource r;mh3_model_catalog_resource(&catalog,0,i,&r);
        if(r.pack_index==UINT32_MAX)goto invalid;
    }
    for(uint32_t i=0;i<catalog.block_count;i++) {
        mh3_catalog_material_block b;mh3_model_catalog_block(&catalog,i,&b);
        if(b.flags!=1 || !b.source_bytes)goto invalid;
    }
    w->sealed=1;*out=catalog;return MH3_PREPARE_OK;
invalid:
    memcpy(w->data,previous,64);return MH3_PREPARE_INVALID;
}
mh3_prepare_status mh3_prepare_abort(mh3_prepare_writer *w) {
    if(!w || !w->data || (w->kind!=1 && w->kind!=2))return MH3_PREPARE_INVALID;
    if(w->kind==1 && w->gpu.truncate(w->gpu.context,0)) {w->sealed=2;return MH3_PREPARE_ROLLBACK;}
    memset(w->data,0,w->kind==1?128:64);w->sealed=2;return MH3_PREPARE_OK;
}
static mh3_prepare_status emit_failure(mh3_prepare_sink sink) {
    return sink.truncate(sink.context,0)?MH3_PREPARE_ROLLBACK:MH3_PREPARE_IO;
}
mh3_prepare_status mh3_prepare_emit(const mh3_prepare_writer *w,mh3_prepare_input input,
    mh3_prepare_sink output,uint8_t *scratch,size_t bytes) {
    if(!w || !w->data || w->kind!=1 || w->sealed!=1 || !input.read || !output.write || !output.truncate ||
       !scratch || !bytes)return MH3_PREPARE_INVALID;
    const uint64_t gpu_at=aligned(w->size,4096),total=gpu_at+w->gpu_bytes;
    if(output.truncate(output.context,0))return emit_failure(output);
    if(output.write(output.context,0,w->data,w->size) || zeros(output,w->size,gpu_at-w->size))return emit_failure(output);
    for(uint64_t at=0;at<w->gpu_bytes;) {
        size_t n=w->gpu_bytes-at<bytes?(size_t)(w->gpu_bytes-at):bytes;
        if(input.read(input.context,at,scratch,n) || output.write(output.context,gpu_at+at,scratch,n))return emit_failure(output);
        at+=n;
    }
    if(output.truncate(output.context,total))return emit_failure(output);
    return MH3_PREPARE_OK;
}
static int path_bytes(const char *p,size_t *length) {
    if(!p)return 0;
    size_t n=0;while(n<=255 && p[n])n++;
    if(!n || n>255 || p[0]=='/' || n<4 || memcmp(p+n-4,".bin",4))return 0;
    size_t component=0;
    for(size_t i=0;i<=n;i++) {
        if(i<n && (p[i]=='\\' || p[i]==':' || (unsigned char)p[i]<32))return 0;
        if(i==n || p[i]=='/') {
            size_t count=i-component;
            if(!count || (i==n && count<=4) || (count==1 && p[component]=='.') ||
               (count==2 && p[component]=='.' && p[component+1]=='.'))return 0;
            component=i+1;
        }
    }
    /* Reject malformed UTF-8 rather than publishing platform-dependent names. */
    for(size_t i=0;i<n;) {
        unsigned char c=(unsigned char)p[i++];if(c<128)continue;
        unsigned tails=c>=0xc2 && c<=0xdf?1:c>=0xe0 && c<=0xef?2:c>=0xf0 && c<=0xf4?3:0;
        if(!tails || tails>n-i || (c==0xe0 && (unsigned char)p[i]<0xa0) ||
           (c==0xed && (unsigned char)p[i]>=0xa0) || (c==0xf0 && (unsigned char)p[i]<0x90) ||
           (c==0xf4 && (unsigned char)p[i]>=0x90))return 0;
        while(tails--)if(((unsigned char)p[i++]&0xc0)!=0x80)return 0;
    }
    *length=n;return 1;
}
mh3_prepare_status mh3_prepare_index(uint8_t *data,size_t capacity,
    const mh3_prepare_index_pack *packs,uint32_t pack_count,
    const mh3_prepare_index_source *sources,uint32_t source_count,size_t *out_bytes) {
    if(!data || !packs || !sources || !out_bytes)return MH3_PREPARE_INVALID;
    if(!pack_count || pack_count>16384 || !source_count || source_count>65536)return MH3_PREPARE_RANGE;
    uint64_t size=64+(uint64_t)pack_count*32+(uint64_t)source_count*24;
    uint8_t used[2048]={0};
    for(uint32_t i=0;i<pack_count;i++) {
        size_t n;const mh3_prepare_index_pack *p=&packs[i];
        if(!path_bytes(p->path,&n) || (i && strcmp(packs[i-1].path,p->path)>=0) ||
           p->metadata_bytes<128 || p->metadata_bytes>MH3_PREPARE_MAX_METADATA ||
           p->identity_bytes<64 || p->identity_bytes>MH3_PREPARE_MAX_IDENTITY ||
           !p->gpu_bytes || p->gpu_bytes>MH3_PREPARE_MAX_GPU ||
           (uint64_t)p->file_bytes!=aligned(p->metadata_bytes,4096)+p->gpu_bytes)return MH3_PREPARE_INVALID;
        size+=n;
    }
    if(size>capacity || size>MH3_PREPARE_MAX_INDEX)return MH3_PREPARE_RANGE;
    for(uint32_t i=0;i<source_count;i++) {
        const mh3_prepare_index_source *s=&sources[i];
        if(!s->source_bytes || s->pack_index>=pack_count || s->partition_offset>UINT64_MAX-s->source_bytes ||
           (i && sources[i-1].partition_offset+sources[i-1].source_bytes>s->partition_offset))return MH3_PREPARE_INVALID;
        used[s->pack_index/8]|=(uint8_t)(1u<<(s->pack_index%8));
    }
    for(uint32_t i=0;i<pack_count;i++)if(!(used[i/8]&(1u<<(i%8))))return MH3_PREPARE_INVALID;
    memset(data,0,(size_t)size);memcpy(data,"M3IX",4);put32(data+4,1);put32(data+8,(uint32_t)size);
    const uint32_t source_at=64+pack_count*32,path_at=source_at+source_count*24;
    put32(data+12,pack_count);put32(data+16,64);put32(data+20,source_count);put32(data+24,source_at);
    put32(data+28,path_at);put32(data+32,(uint32_t)size-path_at);
    uint32_t cursor=path_at;
    for(uint32_t i=0;i<pack_count;i++) {
        const mh3_prepare_index_pack *p=&packs[i];size_t n=strlen(p->path);uint8_t *r=data+64+(size_t)i*32;
        put32(r,cursor);put32(r+4,(uint32_t)n);put32(r+8,p->metadata_bytes);put32(r+12,p->identity_bytes);
        put32(r+16,p->gpu_bytes);put32(r+20,p->file_bytes);memcpy(data+cursor,p->path,n);cursor+=(uint32_t)n;
    }
    for(uint32_t i=0;i<source_count;i++) {
        uint8_t *r=data+source_at+(size_t)i*24;put64(r,sources[i].partition_offset);
        put32(r+8,sources[i].source_bytes);put32(r+12,sources[i].pack_index);
    }
    *out_bytes=(size_t)size;return MH3_PREPARE_OK;
}
