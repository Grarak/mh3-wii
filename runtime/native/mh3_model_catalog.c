#include "mh3_model_catalog.h"
#include <limits.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t u64(const uint8_t *p) {return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32;}
static int extent(size_t size,size_t offset,size_t bytes) {return offset<=size && bytes<=size-offset;}
static int table(mh3_pack_span bytes,uint32_t offset,uint32_t count,uint32_t stride) {
    return offset>=64 && !(offset&7) && offset<=bytes.size && count<=(bytes.size-offset)/stride;
}
mh3_pack_status mh3_model_catalog_model(const mh3_model_catalog *c,uint32_t index,
                                        mh3_catalog_model *out) {
    if (!c || !out || index>=c->model_count) return MH3_PACK_RANGE;
    const uint8_t *p=c->bytes.data+c->model_offset+(size_t)index*64;
    mh3_catalog_model r;
    r.partition_offset=u64(p);r.file_bytes=u32(p+8);r.model_offset=u32(p+12);
    r.model_bytes=u32(p+16);r.model_id=u32(p+20);r.pack_index=u32(p+24);
    r.revision=u32(p+28);r.parent_offset=u32(p+32);r.shape_first=u32(p+36);
    r.shape_count=u32(p+40);r.material_first=u32(p+44);r.material_count=u32(p+48);
    *out=r;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_resource(const mh3_model_catalog *c,int material,
                                           uint32_t index,mh3_catalog_resource *out) {
    if (!c || !out || (material!=0 && material!=1) ||
        index>=(material?c->material_count:c->shape_count)) return MH3_PACK_RANGE;
    const uint8_t *p=c->bytes.data+(material?c->material_offset:c->shape_offset)+(size_t)index*24;
    mh3_catalog_resource r={u32(p),u32(p+4),u32(p+8),u32(p+12),u32(p+16),u32(p+20)};
    *out=r;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_block(const mh3_model_catalog *c,uint32_t index,mh3_catalog_material_block *out) {
    if (!c || !out) return MH3_PACK_INVALID;
    if (c->version!=2) return MH3_PACK_UNSUPPORTED;
    if (index>=c->block_count) return MH3_PACK_RANGE;
    const uint8_t *p=c->bytes.data+c->block_offset+(size_t)index*24;
    mh3_catalog_material_block b={u32(p),u32(p+4),u32(p+8),u32(p+12),u32(p+16)};
    *out=b;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_material_block_find(const mh3_model_catalog *c,uint32_t material,
    uint32_t kind,mh3_catalog_material_block *out) {
    if (!c || !out) return MH3_PACK_INVALID;
    if (c->version!=2) return MH3_PACK_UNSUPPORTED;
    if (material>=c->material_count || kind>=MH3_CATALOG_MATERIAL_BLOCKS) return MH3_PACK_RANGE;
    mh3_catalog_material_block b;
    mh3_pack_status status=mh3_model_catalog_block(c,material*MH3_CATALOG_MATERIAL_BLOCKS+kind,&b);
    if (status!=MH3_PACK_OK) return status;
    if (!(b.flags&1)) return MH3_PACK_UNSUPPORTED;
    *out=b;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_open(mh3_pack_span bytes,mh3_model_catalog *out) {
    if (!out || !bytes.data || bytes.size<64 || bytes.size>UINT32_MAX) return MH3_PACK_RANGE;
    const uint8_t *p=bytes.data;
    if (memcmp(p,"M3ID",4)) return MH3_PACK_INVALID;
    if (u32(p+4)!=1 && u32(p+4)!=2) return MH3_PACK_UNSUPPORTED;
    if (u32(p+8)!=bytes.size) return MH3_PACK_RANGE;
    mh3_model_catalog c={bytes,u32(p+12),u32(p+16),u32(p+20),u32(p+24),u32(p+28),u32(p+32),u32(p+4),u32(p+36),u32(p+40)};
    for (uint32_t i=c.version==1?36:44;i<64;i+=4) if (u32(p+i)) return MH3_PACK_INVALID;
    if (!table(bytes,c.model_offset,c.model_count,64) ||
        !table(bytes,c.shape_offset,c.shape_count,24) ||
        !table(bytes,c.material_offset,c.material_count,24) ||
        (c.version==2 && !table(bytes,c.block_offset,c.block_count,24))) return MH3_PACK_RANGE;
    /* Canonical tightly packed tables prohibit aliasing, orphan tails and
     * inconsistent sizes without overflowing a 32-bit host's multiplication. */
    if (c.model_offset!=64 || (uint64_t)c.shape_offset!=64+(uint64_t)c.model_count*64 ||
        (uint64_t)c.material_offset!=(uint64_t)c.shape_offset+(uint64_t)c.shape_count*24) return MH3_PACK_INVALID;
    const uint64_t resource_end=(uint64_t)c.material_offset+(uint64_t)c.material_count*24;
    if (c.version==1?bytes.size!=resource_end:
        ((uint64_t)c.block_count!=(uint64_t)c.material_count*MH3_CATALOG_MATERIAL_BLOCKS ||
         c.block_offset!=resource_end || bytes.size!=(uint64_t)c.block_offset+(uint64_t)c.block_count*24))
        return MH3_PACK_INVALID;
    uint32_t shape_first=0,material_first=0;
    mh3_catalog_model previous={0};
    for (uint32_t i=0;i<c.model_count;i++) {
        mh3_catalog_model m;mh3_model_catalog_model(&c,i,&m);
        if (!m.file_bytes || !m.model_bytes || !m.model_id || m.revision!=11 || m.parent_offset!=0 ||
            m.partition_offset>UINT64_MAX-m.file_bytes ||
            !extent(m.file_bytes,m.model_offset,m.model_bytes) ||
            m.shape_first!=shape_first || m.material_first!=material_first ||
            !extent(c.shape_count,m.shape_first,m.shape_count) ||
            !extent(c.material_count,m.material_first,m.material_count)) return MH3_PACK_INVALID;
        if (i && (m.partition_offset<previous.partition_offset ||
            (m.partition_offset==previous.partition_offset &&
             (m.model_offset<=previous.model_offset || m.file_bytes!=previous.file_bytes)) ||
            (m.partition_offset!=previous.partition_offset &&
             m.partition_offset<previous.partition_offset+previous.file_bytes))) return MH3_PACK_INVALID;
        p=bytes.data+c.model_offset+(size_t)i*64;
        if (u32(p+52)||u32(p+56)||u32(p+60)) return MH3_PACK_INVALID;
        for (int material=0;material<2;material++) {
            const uint32_t first=material?m.material_first:m.shape_first;
            const uint32_t count=material?m.material_count:m.shape_count;
            uint32_t last=0;
            for (uint32_t j=0;j<count;j++) {
                mh3_catalog_resource r;mh3_model_catalog_resource(&c,material,first+j,&r);
                if (r.model_index!=i || !r.source_bytes || !r.asset_id ||
                    (j && r.source_id<=last) || r.source_offset<m.model_offset ||
                    !extent(m.model_bytes,(size_t)r.source_offset-m.model_offset,r.source_bytes)) return MH3_PACK_INVALID;
                last=r.source_id;
            }
        }
        shape_first+=m.shape_count;material_first+=m.material_count;previous=m;
    }
    if (shape_first!=c.shape_count || material_first!=c.material_count) return MH3_PACK_INVALID;
    if (c.version==2) for (uint32_t i=0;i<c.block_count;i++) {
        mh3_catalog_material_block b;mh3_catalog_resource material;mh3_catalog_model model;
        mh3_model_catalog_block(&c,i,&b);
        if (b.material_index!=i/MH3_CATALOG_MATERIAL_BLOCKS || b.kind!=i%MH3_CATALOG_MATERIAL_BLOCKS ||
            b.flags>1 || u32(bytes.data+c.block_offset+(size_t)i*24+20)) return MH3_PACK_INVALID;
        if (!b.flags) {if (b.source_offset || b.source_bytes) return MH3_PACK_INVALID;continue;}
        mh3_model_catalog_resource(&c,1,b.material_index,&material);
        mh3_model_catalog_model(&c,material.model_index,&model);
        if (b.source_offset<model.model_offset ||
            !extent(model.model_bytes,(size_t)b.source_offset-model.model_offset,b.source_bytes)) return MH3_PACK_INVALID;
        if (b.kind<4) {
            static const uint32_t sizes[4]={32,128,64,160},offsets[4]={0,32,160,224};
            if (b.source_bytes!=sizes[b.kind] || b.source_offset<material.source_offset ||
                !extent(material.source_bytes,(size_t)b.source_offset-material.source_offset,b.source_bytes)) return MH3_PACK_INVALID;
            mh3_catalog_material_block pixel;mh3_model_catalog_block(&c,i-b.kind,&pixel);
            if ((pixel.flags&1) && (uint64_t)b.source_offset!=(uint64_t)pixel.source_offset+offsets[b.kind]) return MH3_PACK_INVALID;
        } else if (b.source_bytes<160 || b.source_bytes>480 || (b.source_bytes&31)) return MH3_PACK_INVALID;
    }
    *out=c;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_find(const mh3_model_catalog *c,uint64_t offset,uint32_t model_offset,
                                       uint32_t *out) {
    if (!c || !out) return MH3_PACK_INVALID;
    uint32_t lo=0,hi=c->model_count;
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;mh3_catalog_model m;mh3_model_catalog_model(c,mid,&m);
        if (m.partition_offset<offset || (m.partition_offset==offset && m.model_offset<model_offset)) lo=mid+1;
        else hi=mid;
    }
    if (lo==c->model_count) return MH3_PACK_RANGE;
    mh3_catalog_model m;mh3_model_catalog_model(c,lo,&m);
    if (m.partition_offset!=offset || m.model_offset!=model_offset) return MH3_PACK_RANGE;
    *out=lo;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_find_resource(const mh3_model_catalog *c,uint32_t model_index,
                                                int material,uint32_t source_id,mh3_catalog_resource *out) {
    mh3_catalog_model m;
    if (!out || (material!=0 && material!=1)) return MH3_PACK_INVALID;
    mh3_pack_status status=mh3_model_catalog_model(c,model_index,&m);
    if (status!=MH3_PACK_OK) return status;
    uint32_t first=material?m.material_first:m.shape_first,lo=0,hi=material?m.material_count:m.shape_count;
    const uint32_t count=hi;
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;mh3_catalog_resource r;mh3_model_catalog_resource(c,material,first+mid,&r);
        if (r.source_id<source_id) lo=mid+1;else hi=mid;
    }
    if (lo==count) return MH3_PACK_RANGE;
    mh3_catalog_resource r;mh3_model_catalog_resource(c,material,first+lo,&r);
    if (r.source_id!=source_id) return MH3_PACK_RANGE;
    *out=r;return MH3_PACK_OK;
}
mh3_pack_status mh3_model_catalog_validate_pack(const mh3_model_catalog *c,const mh3_mesh_pack *pack) {
    if (!c || !pack || c->model_count!=pack->counts[MH3_PACK_MODELS]) return MH3_PACK_INVALID;
    for (uint32_t i=0;i<c->model_count;i++) {
        mh3_catalog_model m;mh3_pack_model p;
        mh3_model_catalog_model(c,i,&m);
        if (mh3_mesh_pack_model(pack,m.pack_index,&p)!=MH3_PACK_OK || p.id!=m.model_id ||
            p.source_offset!=m.model_offset || p.revision!=m.revision || p.material_count!=m.material_count)
            return MH3_PACK_INVALID;
        for (uint32_t j=0;j<i;j++) {
            mh3_catalog_model other;mh3_model_catalog_model(c,j,&other);
            if (other.pack_index==m.pack_index || other.model_id==m.model_id) return MH3_PACK_INVALID;
        }
        uint32_t converted=0;
        for (int material=0;material<2;material++) {
            uint32_t first=material?m.material_first:m.shape_first,count=material?m.material_count:m.shape_count;
            for (uint32_t j=0;j<count;j++) {
                mh3_catalog_resource r;mh3_model_catalog_resource(c,material,first+j,&r);
                if (material) {
                    mh3_pack_material mat;
                    if (r.pack_index<p.material_first || r.pack_index-p.material_first>=p.material_count ||
                        mh3_mesh_pack_material(pack,r.pack_index,&mat)!=MH3_PACK_OK || mat.model_id!=m.model_id ||
                        mat.source_id!=r.source_id || mat.asset_id!=r.asset_id) return MH3_PACK_INVALID;
                    if (c->version==2) {
                        mh3_catalog_material_block block;
                        mh3_pack_status s=mh3_model_catalog_material_block_find(c,first+j,4,&block);
                        if (s==MH3_PACK_OK) {
                            const uint32_t emitted=(96+((mat.tev_count+1)/2)*48+31)&~31u;
                            if (!mat.tev_count || mat.tev_count>16 || block.source_bytes!=emitted) return MH3_PACK_INVALID;
                        } else if (s!=MH3_PACK_UNSUPPORTED) return s;
                    }
                } else if (r.pack_index!=UINT32_MAX) {
                    mh3_pack_mesh mesh;
                    if (r.pack_index<p.mesh_first || r.pack_index-p.mesh_first>=p.mesh_count ||
                        mh3_mesh_pack_mesh(pack,r.pack_index,&mesh)!=MH3_PACK_OK || mesh.model_id!=m.model_id ||
                        mesh.source_id!=r.source_id || mesh.asset_id!=r.asset_id) return MH3_PACK_INVALID;
                    converted++;
                }
            }
        }
        if (converted!=p.mesh_count) return MH3_PACK_INVALID;
    }
    return MH3_PACK_OK;
}

static int memory_range(uint32_t address,uint32_t bytes) {
    return bytes && (uint64_t)address+bytes<=UINT64_C(0x100000000);
}
mh3_res_status mh3_source_owner_certify(const mh3_source_tracker *tracker,
    const mh3_model_catalog *catalog,uint32_t model_address,mh3_source_owner *out) {
    if (!tracker || !catalog || !out) return MH3_RES_INVALID;
    mh3_source_owner owner;memset(&owner,0,sizeof owner);owner.model_address=model_address;
    mh3_res_status status=mh3_source_resolve_model(tracker,catalog,model_address,
        &owner.catalog_index,&owner.container_address,&owner.load_generation);
    if (status!=MH3_RES_OK) return status;
    mh3_catalog_model model;
    if (mh3_model_catalog_model(catalog,owner.catalog_index,&model)!=MH3_PACK_OK) return MH3_RES_INVALID;
    owner.file_bytes=model.file_bytes;*out=owner;return MH3_RES_OK;
}
int mh3_source_owner_intersects(const mh3_source_owner *owner,uint32_t address,uint32_t bytes) {
    if (!owner || !memory_range(address,bytes) || !memory_range(owner->container_address,owner->file_bytes)) return 0;
    return (uint64_t)address+bytes>owner->container_address &&
        (uint64_t)owner->container_address+owner->file_bytes>address;
}
mh3_res_status mh3_source_owner_sibling(const mh3_source_owner *owner,
    const mh3_model_catalog *catalog,uint32_t index,mh3_source_owner *out) {
    if(!owner || !catalog || !out || !owner->load_generation ||
       !memory_range(owner->container_address,owner->file_bytes)) return MH3_RES_INVALID;
    mh3_catalog_model original,sibling;
    if(mh3_model_catalog_model(catalog,owner->catalog_index,&original)!=MH3_PACK_OK ||
       original.file_bytes!=owner->file_bytes ||
       (uint64_t)owner->container_address+original.model_offset!=owner->model_address)
        return MH3_RES_INVALID;
    if(mh3_model_catalog_model(catalog,index,&sibling)!=MH3_PACK_OK) return MH3_RES_RANGE;
    if(sibling.partition_offset!=original.partition_offset || sibling.file_bytes!=original.file_bytes)
        return MH3_RES_ABSENT;
    mh3_source_owner result=*owner;result.catalog_index=index;
    result.model_address=owner->container_address+sibling.model_offset;*out=result;return MH3_RES_OK;
}
mh3_res_status mh3_source_owner_material_write(const mh3_source_owner *owner,
    const mh3_model_catalog *catalog,uint32_t address,uint32_t bytes,uint32_t kind) {
    if (!owner || !catalog || !memory_range(address,bytes) || kind>=MH3_CATALOG_MATERIAL_BLOCKS) return MH3_RES_INVALID;
    if (!mh3_source_owner_intersects(owner,address,bytes)) return MH3_RES_ABSENT;
    if (catalog->version!=2) return MH3_RES_UNSUPPORTED;
    mh3_catalog_model model;
    if (mh3_model_catalog_model(catalog,owner->catalog_index,&model)!=MH3_PACK_OK || model.file_bytes!=owner->file_bytes ||
        (uint64_t)owner->container_address+model.model_offset!=owner->model_address) return MH3_RES_INVALID;
    /* Several MDL0 resources can share a BRRES allocation. A typed write in
     * any certified material block preserves every model's allocation owner,
     * without allowing writes to the container or resource layout headers. */
    uint32_t first_model=owner->catalog_index;
    while (first_model) {
        mh3_catalog_model preceding;
        if (mh3_model_catalog_model(catalog,first_model-1,&preceding)!=MH3_PACK_OK) return MH3_RES_INVALID;
        if (preceding.partition_offset!=model.partition_offset) break;
        first_model--;
    }
    for (uint32_t j=first_model;j<catalog->model_count;j++) {
        mh3_catalog_model same_file;
        if (mh3_model_catalog_model(catalog,j,&same_file)!=MH3_PACK_OK) return MH3_RES_INVALID;
        if (same_file.partition_offset!=model.partition_offset) break;
        if (same_file.file_bytes!=owner->file_bytes) return MH3_RES_INVALID;
        for (uint32_t i=0;i<same_file.material_count;i++) {
            mh3_catalog_material_block block;
            const mh3_pack_status status=mh3_model_catalog_material_block_find(catalog,same_file.material_first+i,kind,&block);
            if (status==MH3_PACK_UNSUPPORTED) continue;
            if (status!=MH3_PACK_OK) return MH3_RES_INVALID;
            const uint64_t first=(uint64_t)owner->container_address+block.source_offset;
            if (address>=first && (uint64_t)address+bytes<=first+block.source_bytes) return MH3_RES_OK;
        }
    }
    return MH3_RES_RANGE;
}
mh3_res_status mh3_source_resolve_material_block(const mh3_source_tracker *tracker,
    const mh3_model_catalog *catalog,uint32_t address,uint32_t material_index,
    uint32_t kind,uint64_t *generation) {
    if (!tracker || !catalog || !generation) return MH3_RES_INVALID;
    mh3_catalog_material_block block;
    const mh3_pack_status status=mh3_model_catalog_material_block_find(catalog,material_index,kind,&block);
    if (status==MH3_PACK_UNSUPPORTED) return MH3_RES_UNSUPPORTED;
    if (status!=MH3_PACK_OK) return MH3_RES_RANGE;
    mh3_catalog_resource material;mh3_catalog_model model;mh3_source_identity source;
    if (mh3_model_catalog_resource(catalog,1,material_index,&material)!=MH3_PACK_OK ||
        mh3_model_catalog_model(catalog,material.model_index,&model)!=MH3_PACK_OK) return MH3_RES_INVALID;
    const mh3_res_status resolved=mh3_source_resolve(tracker,address,block.source_bytes,&source);
    if (resolved!=MH3_RES_OK) return resolved;
    if (source.partition_offset!=model.partition_offset+block.source_offset) return MH3_RES_ABSENT;
    *generation=source.generation;return MH3_RES_OK;
}
mh3_res_status mh3_source_tracker_init(mh3_source_tracker *out,mh3_source_range *ranges,
                                      mh3_source_range *work,mh3_source_range *transfer,size_t capacity) {
    if (!out || !ranges || !work || !transfer || !capacity || capacity>SIZE_MAX/sizeof(*ranges) ||
        ranges==work || ranges==transfer || work==transfer) return MH3_RES_INVALID;
    mh3_source_tracker t={ranges,work,transfer,0,capacity,0};*out=t;return MH3_RES_OK;
}
static mh3_res_status append(mh3_source_tracker *t,size_t *count,mh3_source_range r) {
    if (*count>=t->capacity) return MH3_RES_RANGE;
    t->work[(*count)++]=r;return MH3_RES_OK;
}
static mh3_res_status remove_range(mh3_source_tracker *t,uint32_t address,uint32_t bytes,size_t *count) {
    uint64_t end=(uint64_t)address+bytes;*count=0;
    for (size_t i=0;i<t->count;i++) {
        mh3_source_range r=t->ranges[i];uint64_t re=(uint64_t)r.address+r.bytes;
        if (re<=address || r.address>=end) {
            if (append(t,count,r)!=MH3_RES_OK) return MH3_RES_RANGE;
            continue;
        }
        if (r.address<address) {
            mh3_source_range left=r;left.bytes=address-r.address;
            if (append(t,count,left)!=MH3_RES_OK) return MH3_RES_RANGE;
        }
        if (re>end) {
            mh3_source_range right=r;right.address=(uint32_t)end;right.bytes=(uint32_t)(re-end);
            right.partition_offset+=end-r.address;
            if (append(t,count,right)!=MH3_RES_OK) return MH3_RES_RANGE;
        }
    }
    return MH3_RES_OK;
}
static void commit(mh3_source_tracker *t,size_t count) {
    /* Ordered nonoverlapping ranges enable load-time binary address lookup. */
    for (size_t i=1;i<count;i++) {
        mh3_source_range r=t->work[i];size_t j=i;
        while(j && t->work[j-1].address>r.address) {t->work[j]=t->work[j-1];j--;}
        t->work[j]=r;
    }
    size_t n=0;
    for (size_t i=0;i<count;i++) {
        mh3_source_range r=t->work[i];
        if (n && (uint64_t)t->ranges[n-1].address+t->ranges[n-1].bytes==r.address &&
            t->ranges[n-1].partition_offset+t->ranges[n-1].bytes==r.partition_offset &&
            t->ranges[n-1].generation==r.generation &&
            (uint64_t)t->ranges[n-1].bytes+r.bytes<=UINT32_MAX) t->ranges[n-1].bytes+=r.bytes;
        else t->ranges[n++]=r;
    }
    t->count=n;
}
mh3_res_status mh3_source_record_disc(mh3_source_tracker *t,uint32_t address,uint32_t bytes,
                                     uint64_t offset,uint64_t generation) {
    if (!t || !memory_range(address,bytes) || generation<=t->generation || offset>UINT64_MAX-bytes) return MH3_RES_INVALID;
    size_t count;
    if (remove_range(t,address,bytes,&count)!=MH3_RES_OK) return MH3_RES_RANGE;
    mh3_source_range r={address,bytes,offset,generation};
    if (append(t,&count,r)!=MH3_RES_OK) return MH3_RES_RANGE;
    commit(t,count);t->generation=generation;return MH3_RES_OK;
}
mh3_res_status mh3_source_retire(mh3_source_tracker *t,uint32_t address,uint32_t bytes) {
    if (!t || !memory_range(address,bytes)) return MH3_RES_INVALID;
    size_t count;if (remove_range(t,address,bytes,&count)!=MH3_RES_OK) return MH3_RES_RANGE;
    commit(t,count);return MH3_RES_OK;
}
int mh3_source_intersects(const mh3_source_tracker *t,uint32_t address,uint32_t bytes) {
    if (!t || !memory_range(address,bytes)) return 0;
    const uint64_t end=(uint64_t)address+bytes;size_t lo=0,hi=t->count;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        if ((uint64_t)t->ranges[mid].address+t->ranges[mid].bytes<=address) lo=mid+1;
        else hi=mid;
    }
    return lo<t->count && t->ranges[lo].address<end;
}
mh3_res_status mh3_source_record_copy(mh3_source_tracker *t,uint32_t dst,uint32_t src,
                                     uint32_t bytes,uint64_t generation) {
    if (!t || !memory_range(dst,bytes) || !memory_range(src,bytes) || generation<=t->generation) return MH3_RES_INVALID;
    const uint64_t end=(uint64_t)src+bytes;size_t n=0;
    for (size_t i=0;i<t->count;i++) {
        const mh3_source_range r=t->ranges[i];uint64_t begin=r.address,re=(uint64_t)r.address+r.bytes;
        if (begin<src) begin=src;
        if (re>end) re=end;
        if (begin>=re) continue;
        if (n>=t->capacity) return MH3_RES_RANGE;
        mh3_source_range copy={(uint32_t)(dst+begin-src),(uint32_t)(re-begin),r.partition_offset+begin-r.address,generation};
        t->transfer[n++]=copy;
    }
    size_t count;if (remove_range(t,dst,bytes,&count)!=MH3_RES_OK) return MH3_RES_RANGE;
    for (size_t i=0;i<n;i++) if (append(t,&count,t->transfer[i])!=MH3_RES_OK) return MH3_RES_RANGE;
    commit(t,count);t->generation=generation;return MH3_RES_OK;
}
mh3_res_status mh3_source_resolve(const mh3_source_tracker *t,uint32_t address,uint32_t bytes,mh3_source_identity *out) {
    if (!t || !out || !memory_range(address,bytes)) return MH3_RES_INVALID;
    uint64_t cursor=address,end=(uint64_t)address+bytes,expected=0,generation=0,start=0;
    for (size_t i=0;i<t->count && cursor<end;i++) {
        const mh3_source_range r=t->ranges[i];const uint64_t re=(uint64_t)r.address+r.bytes;
        if (re<=cursor) continue;
        if (r.address>cursor) return MH3_RES_ABSENT;
        const uint64_t offset=r.partition_offset+cursor-r.address;
        if (!generation) start=offset;
        else if (offset!=expected) return MH3_RES_ABSENT;
        if (r.generation>generation) generation=r.generation;
        uint64_t next=re<end?re:end;expected=offset+next-cursor;cursor=next;
    }
    if (cursor!=end) return MH3_RES_ABSENT;
    mh3_source_identity id={start,generation};*out=id;return MH3_RES_OK;
}
mh3_res_status mh3_source_resolve_model(const mh3_source_tracker *t,const mh3_model_catalog *c,
                                       uint32_t address,uint32_t *index,uint32_t *base,uint64_t *generation) {
    if (!c || !index || !base || !generation) return MH3_RES_INVALID;
    mh3_source_identity mdl;
    mh3_res_status s=mh3_source_resolve(t,address,1,&mdl);if (s!=MH3_RES_OK) return s;
    /* Model count is small per area. The catalog's disc ordering permits a
     * future binary file-range lookup without a mutable address/name index. */
    for (uint32_t i=0;i<c->model_count;i++) {
        mh3_catalog_model m;mh3_model_catalog_model(c,i,&m);
        if (mdl.partition_offset!=m.partition_offset+m.model_offset) continue;
        if (address<m.model_offset) return MH3_RES_RANGE;
        const uint32_t owner=address-m.model_offset;mh3_source_identity file;
        s=mh3_source_resolve(t,owner,m.file_bytes,&file);if (s!=MH3_RES_OK) return s;
        if (file.partition_offset!=m.partition_offset) return MH3_RES_ABSENT;
        *index=i;*base=owner;*generation=file.generation;return MH3_RES_OK;
    }
    return MH3_RES_ABSENT;
}
void *mh3_model_copy_bytes(void *destination,const void *source,size_t bytes) {
    return bytes?memmove(destination,source,bytes):destination;
}
mh3_res_status mh3_source_catalog_live(const mh3_source_tracker *tracker,const mh3_model_catalog *catalog,int *out) {
    if(!tracker || !catalog || !out || tracker->count>tracker->capacity || (tracker->count && !tracker->ranges)) return MH3_RES_INVALID;
    for(size_t j=0;j<tracker->count;j++)
        if(tracker->ranges[j].partition_offset>UINT64_MAX-tracker->ranges[j].bytes) return MH3_RES_INVALID;
    int live=0;
    for(uint32_t i=0;i<catalog->model_count;i++) {
        mh3_catalog_model model;
        if(mh3_model_catalog_model(catalog,i,&model)!=MH3_PACK_OK || model.partition_offset>UINT64_MAX-model.file_bytes) return MH3_RES_INVALID;
        for(size_t j=0;j<tracker->count;j++) {
            const mh3_source_range *range=&tracker->ranges[j];
            if(range->bytes && model.file_bytes && range->partition_offset<model.partition_offset+model.file_bytes &&
               model.partition_offset<range->partition_offset+range->bytes) live=1;
        }
    }
    *out=live;return MH3_RES_OK;
}
mh3_res_status mh3_source_canonical_address(uint32_t address,uint32_t bytes,uint32_t *out) {
    if(!out) return MH3_RES_INVALID;
    const uint32_t high=address&0xc0000000u,physical=address&0x3fffffffu;
    if(high!=0 && high!=0x80000000u && high!=0xc0000000u) return MH3_RES_RANGE;
    uint32_t end;
    if(physical<0x01800000u) end=0x01800000u;
    else if(physical>=0x10000000u && physical<0x14000000u) end=0x14000000u;
    else return MH3_RES_RANGE;
    if(bytes>end-physical) return MH3_RES_RANGE;
    *out=physical|0x80000000u;return MH3_RES_OK;
}
