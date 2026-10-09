/* RMHP08 802AE5C8/802AE624, checked against Ghidra C and Gekko listings.
 * Ordinary C owns table traversal and resource-request ordering. */
#include "mh3_field_resources.h"

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

uint32_t mh3_field_resources_descriptor(uint8_t map, uint8_t kind,
    uint32_t primary, uint32_t alternate) {
    return map==kind ? primary : alternate;
}

mh3_field_res_status mh3_field_resources_area(mh3_field_res_bytes descriptor,
    mh3_field_res_bytes areas, uint8_t area, uint32_t *record) {
    if(!record || !descriptor.data) return MH3_FIELD_RES_INVALID;
    if(descriptor.bytes<8 || area>=descriptor.data[0]) return MH3_FIELD_RES_RANGE;
    if(!areas.data || (size_t)area*4+4>areas.bytes) return MH3_FIELD_RES_RANGE;
    const uint32_t selected=word(areas.data+(size_t)area*4);
    if(!selected) return MH3_FIELD_RES_INVALID;
    *record=selected;
    return MH3_FIELD_RES_OK;
}

static mh3_field_res_status pair(const mh3_field_res_table *table,
    size_t index, mh3_field_res_pair *out) {
    if(!table->records.data || index>SIZE_MAX/8) return MH3_FIELD_RES_RANGE;
    const size_t offset=index*8;
    if(offset>table->records.bytes || table->records.bytes-offset<8)
        return MH3_FIELD_RES_RANGE;
    out->bytes=word(table->records.data+offset);
    out->path=word(table->records.data+offset+4);
    return MH3_FIELD_RES_OK;
}

static mh3_field_res_status effects(const mh3_field_res_table *table,
    uint8_t map, uint8_t area, uint8_t *ids, size_t capacity,
    const mh3_field_res_callbacks *c) {
    if(!table->records.data) return MH3_FIELD_RES_OK;
    mh3_field_res_pair row;
    mh3_field_res_status status=pair(table,0,&row);
    if(status!=MH3_FIELD_RES_OK || !row.bytes) return status;
    status=pair(table,1,&row);
    if(status!=MH3_FIELD_RES_OK || !row.bytes) return status;
    size_t count=0;
    if(row.path) {
        if(capacity<2) return MH3_FIELD_RES_RANGE;
        const uint8_t id=c->request(c->user,2,map,area,1);
        if(id==UINT8_MAX) return MH3_FIELD_RES_OK;
        ids[count++]=id;
    }
    for(size_t index=2;;++index) {
        status=pair(table,index,&row);
        if(status!=MH3_FIELD_RES_OK) return status;
        /* The original compares the running index with this row's first
         * word, rather than interpreting that word as a global count. */
        if(index>=row.bytes) break;
        if(!row.path) continue;
        if(count>=capacity-1) return MH3_FIELD_RES_RANGE;
        const uint8_t id=c->request(c->user,1,map,area,(uint8_t)index);
        if(id==UINT8_MAX) {
            for(size_t i=0;i<count;++i) c->release(c->user,ids[i]);
            return MH3_FIELD_RES_OK;
        }
        ids[count++]=id;
    }
    if(count>=capacity) return MH3_FIELD_RES_RANGE;
    ids[count]=UINT8_MAX;
    c->publish(c->user,0,table->address,ids,count+1);
    return MH3_FIELD_RES_OK;
}

static mh3_field_res_status extras(const mh3_field_res_table *table,
    uint8_t map, uint8_t area, uint8_t *ids, size_t capacity,
    const mh3_field_res_callbacks *c) {
    if(!table->records.data) return MH3_FIELD_RES_OK;
    size_t count=0;
    for(;;++count) {
        mh3_field_res_pair row;
        const mh3_field_res_status status=pair(table,count,&row);
        if(status!=MH3_FIELD_RES_OK) return status;
        if(!row.bytes) break;
        if(count>=capacity) return MH3_FIELD_RES_RANGE;
        if(row.path) ids[count]=c->request(c->user,3,map,area,(uint8_t)count);
    }
    c->publish(c->user,3,table->address,ids,count);
    return MH3_FIELD_RES_OK;
}

mh3_field_res_status mh3_field_resources_schedule(uint8_t mode, uint8_t map,
    uint8_t area, uint8_t *ids, size_t id_bytes,
    const mh3_field_res_callbacks *c) {
    if(!ids || !id_bytes || !c || !c->table || !c->primary || !c->online_mode ||
       !c->online_prepare || !c->environment_prepare || !c->request ||
       !c->release || !c->publish) return MH3_FIELD_RES_INVALID;
    mh3_field_res_table table={0};
    mh3_field_res_status status=c->table(c->user,MH3_FIELD_RES_PRIMARY,map,area,&table);
    if(status!=MH3_FIELD_RES_OK) return status;
    for(unsigned part=0;part<2;++part) {
        mh3_field_res_pair row;
        status=pair(&table,part,&row);
        if(status!=MH3_FIELD_RES_OK) return status;
        if(row.bytes) c->primary(c->user,&row,mode,map,area,(uint8_t)part);
    }
    if(c->online_mode(c->user)==2) c->online_prepare(c->user,map,area);
    c->environment_prepare(c->user,map,area);
    status=c->table(c->user,MH3_FIELD_RES_EFFECT,map,area,&table);
    if(status!=MH3_FIELD_RES_OK) return status;
    status=effects(&table,map,area,ids,id_bytes,c);
    if(status!=MH3_FIELD_RES_OK) return status;
    status=c->table(c->user,MH3_FIELD_RES_EXTRA,map,area,&table);
    if(status!=MH3_FIELD_RES_OK) return status;
    return extras(&table,map,area,ids,id_bytes,c);
}
