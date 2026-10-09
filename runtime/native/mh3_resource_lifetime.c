#include "mh3_resource_lifetime.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0]<<8|p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static void put16(uint8_t *p,uint16_t value) { p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value; }
static void put32(uint8_t *p,uint32_t value) {
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);
    p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}
static int block_in_heap(const mh3_lifetime_expheap *heap,uint32_t address) {
    return address>=heap->begin && (uint64_t)address+16<=heap->end && !(address&3);
}
mh3_lifetime_status mh3_lifetime_expheap_read(const void *data,size_t bytes,
    uint32_t address,mh3_lifetime_expheap *out) {
    if(!data||!out) return MH3_LIFETIME_INVALID;
    if(bytes<80) return MH3_LIFETIME_BOUNDS;
    const uint8_t *p=data;
    mh3_lifetime_expheap result={address,be32(p+24),be32(p+28),be32(p+56),
        be32(p+60),be32(p+64),be32(p+68),be32(p+72)};
    if(be32(p)!=UINT32_C(0x45585048)|| (address&3)||
       (uint64_t)address+80>result.begin||result.begin>result.end)
        return MH3_LIFETIME_INVALID;
    const uint32_t links[]={result.free_head,result.free_tail,result.used_head,result.used_tail};
    for(unsigned i=0;i<4;i++) if(links[i]&&!block_in_heap(&result,links[i]))
        return MH3_LIFETIME_BOUNDS;
    *out=result;return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_expblock_read(const void *data,size_t bytes,
    mh3_lifetime_expblock *out) {
    if(!data||!out) return MH3_LIFETIME_INVALID;
    if(bytes<16) return MH3_LIFETIME_BOUNDS;
    const uint8_t *p=data;
    *out=(mh3_lifetime_expblock){be16(p),be16(p+2),be32(p+4),be32(p+8),be32(p+12)};
    return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_expblock_extent(const mh3_lifetime_expheap *heap,
    uint32_t payload,const mh3_lifetime_expblock *block,mh3_lifetime_range *out) {
    if(!heap||!block||!out||payload<16||block->state!=0x5544) return MH3_LIFETIME_INVALID;
    const uint32_t header=payload-16,padding=(block->settings>>8)&127;
    if(!block_in_heap(heap,header)||header<padding||header-padding<heap->begin||
       (uint64_t)payload+block->size>heap->end||
       (uint64_t)16+padding+block->size>UINT32_MAX) return MH3_LIFETIME_BOUNDS;
    *out=(mh3_lifetime_range){header-padding,16+padding+block->size};
    return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_expheap_free_plan(const mh3_lifetime_expheap *heap,
    uint32_t payload,const mh3_lifetime_expblock *block,mh3_lifetime_free_plan *out) {
    if(!out) return MH3_LIFETIME_INVALID;
    mh3_lifetime_free_plan result={0};
    mh3_lifetime_status status=mh3_lifetime_expblock_extent(heap,payload,block,&result.retired);
    if(status!=MH3_LIFETIME_OK) return status;
    const uint32_t header=payload-16;
    if((block->previous&&!block_in_heap(heap,block->previous))||
       (block->next&&!block_in_heap(heap,block->next))||
       block->previous==header||block->next==header||
       (!block->previous&&heap->used_head!=header)||(!block->next&&heap->used_tail!=header))
        return MH3_LIFETIME_INVALID;
    result.region_begin=result.retired.address;
    result.region_end=payload+block->size;
    result.links[0]=(mh3_lifetime_link_write){block->previous?block->previous+12:heap->address+68,block->next};
    result.links[1]=(mh3_lifetime_link_write){block->next?block->next+8:heap->address+72,block->previous};
    *out=result;return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_expheap_create_plan(uint32_t address,uint32_t bytes,
    mh3_lifetime_range *out) {
    if(!out) return MH3_LIFETIME_INVALID;
    const uint32_t begin=(address+3)&~UINT32_C(3),end=(address+bytes)&~UINT32_C(3);
    /* Preserve the SDK's failed-small/wrapped-region return instead of
     * certifying or touching a nonexistent allocation. */
    *out=end<begin||end-begin<100 ? (mh3_lifetime_range){0,0}:
        (mh3_lifetime_range){begin,end-begin};
    return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_expheap_initialize(void *heap,size_t heap_bytes,
    void *free_block,size_t block_bytes,uint32_t begin,uint32_t end) {
    if(!heap||!free_block||begin>end||end-begin<20) return MH3_LIFETIME_INVALID;
    if(heap_bytes<80||block_bytes<16) return MH3_LIFETIME_BOUNDS;
    uint8_t *h=heap,*b=free_block;
    put16(h+76,0);put16(h+78,0);
    put16(b,0x4652);put16(b+2,0);put32(b+4,end-begin-16);
    put32(b+8,0);put32(b+12,0);
    put32(h+60,begin);put32(h+64,begin);put32(h+68,0);put32(h+72,0);
    return MH3_LIFETIME_OK;
}
uint32_t mh3_lifetime_expheap_request_size(uint32_t size) { return ((size?size:1)+3)&~UINT32_C(3); }
uint32_t mh3_lifetime_alignment_magnitude(int32_t alignment) {
    return alignment<0 ? 0u-(uint32_t)alignment:(uint32_t)alignment;
}
mh3_lifetime_status mh3_lifetime_expheap_available(const mh3_lifetime_expheap *heap,
    int32_t alignment,mh3_lifetime_read_block read,void *context,uint32_t *out) {
    if(!heap||!read||!out||heap->begin>heap->end) return MH3_LIFETIME_INVALID;
    const uint32_t align=mh3_lifetime_alignment_magnitude(alignment);
    uint32_t address=heap->free_head,largest=0,least_padding=UINT32_MAX;
    uint32_t remaining=(heap->end-heap->begin)/16;
    while(address) {
        if(!remaining--||!block_in_heap(heap,address)) return MH3_LIFETIME_INVALID;
        mh3_lifetime_expblock block;
        mh3_lifetime_status status=read(context,address,&block);
        if(status!=MH3_LIFETIME_OK) return status;
        const uint32_t start=address+16;
        if(block.state!=0x4652||(uint64_t)start+block.size>heap->end) return MH3_LIFETIME_BOUNDS;
        const uint32_t end=start+block.size,aligned=(start+align-1)&~(align-1);
        if(aligned<end) {
            const uint32_t bytes=end-aligned,padding=aligned-start;
            if(bytes>largest||(bytes==largest&&padding<least_padding)) {largest=bytes;least_padding=padding;}
        }
        address=block.next;
    }
    *out=largest;return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_validate_copy32(uint32_t destination,uint32_t source,uint32_t bytes) {
    return !destination||!source||(destination&3)||(source&3)||(bytes&31)?MH3_LIFETIME_INVALID:MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_validate_zero32(uint32_t destination,uint32_t bytes) {
    return !destination||(destination&3)||(bytes&31)?MH3_LIFETIME_INVALID:MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_forward_copy_kind(uint32_t destination,uint32_t source,
    uint32_t bytes,mh3_lifetime_copy_kind *out) {
    if(!out) return MH3_LIFETIME_INVALID;
    if((uint64_t)destination+bytes>UINT64_C(0x100000000)||
       (uint64_t)source+bytes>UINT64_C(0x100000000))
        return MH3_LIFETIME_BOUNDS;
    *out=!bytes||destination==source?MH3_LIFETIME_COPY_UNCHANGED:
        destination>source&&destination-source<bytes?MH3_LIFETIME_COPY_UNKNOWN:MH3_LIFETIME_COPY_SOURCE;
    return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_copy32(void *destination,size_t destination_bytes,
    const void *source,size_t source_bytes,uint32_t bytes) {
    if(bytes&31) return MH3_LIFETIME_INVALID;
    if(bytes>destination_bytes||bytes>source_bytes) return MH3_LIFETIME_BOUNDS;
    if(!bytes) return MH3_LIFETIME_OK;
    if(!destination||!source) return MH3_LIFETIME_INVALID;
    uint8_t *dst=destination;const uint8_t *src=source;
    for(uint32_t i=0;i<bytes;i+=8) {uint8_t unit[8];memcpy(unit,src+i,8);memcpy(dst+i,unit,8);}
    return MH3_LIFETIME_OK;
}
mh3_lifetime_status mh3_lifetime_fill(void *destination,size_t destination_bytes,uint8_t value,uint32_t bytes) {
    if(bytes>destination_bytes) return MH3_LIFETIME_BOUNDS;
    if(!bytes) return MH3_LIFETIME_OK;
    if(!destination) return MH3_LIFETIME_INVALID;
    memset(destination,value,bytes);return MH3_LIFETIME_OK;
}
