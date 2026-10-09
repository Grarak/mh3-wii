#include "mh3_prepare_model.h"
#include <math.h>
#include <string.h>

enum {MAX_MODELS=4096,MAX_TEXTURES=32768,MAX_STRINGS=65536};
typedef struct {
    const mh3_prepare_model_workspace *w;
    mh3_prepare_writer mesh,identity;uint32_t cursor[8],layouts,strings;
    const mh3_prepare_texture *textures;uint32_t texture_count;
    mh3_prepare_model_diagnostic diagnostic;mh3_prepare_status status;
} preparation;
static uint16_t be16(const uint8_t *p) {return (uint16_t)((uint16_t)p[0]<<8|p[1]);}
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void le16(uint8_t *p,uint16_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void le32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void le64(uint8_t *p,uint64_t v) {le32(p,(uint32_t)v);le32(p+4,(uint32_t)(v>>32));}
static int fits(size_t bytes,size_t at,size_t n) {return at<=bytes && n<=bytes-at;}
static int model_fits(const mh3_res_model *m,size_t at,size_t n) {return at>=m->offset && fits(m->size,at-m->offset,n);}
static mh3_prepare_status fail(preparation *p,mh3_prepare_status s) {if(!p->status)p->status=s;return p->status;}
static const char *name(mh3_res_span span,size_t base,size_t field,int optional) {
    size_t at;mh3_res_status s=mh3_res_relative(span,base,field,1,&at);
    if(s==MH3_RES_ABSENT && optional)return NULL;
    if(s!=MH3_RES_OK)return NULL;
    const uint8_t *end=memchr(span.data+at,0,span.size-at>256?256:span.size-at);
    if(!end || end==span.data+at)return NULL;
    return (const char *)(span.data+at);
}
static int text(const char *s) {
    if(!s)return 0;
    size_t n=0;while(n<256 && s[n])n++;if(!n || n==256)return 0;
    for(size_t i=0;i<n;) {
        unsigned a=(uint8_t)s[i++];if(a<128)continue;
        unsigned k=a>=0xc2 && a<=0xdf?1:a>=0xe0 && a<=0xef?2:a>=0xf0 && a<=0xf4?3:0;
        if(!k || k>n-i || (a==0xe0 && (uint8_t)s[i]<0xa0) || (a==0xed && (uint8_t)s[i]>=0xa0) ||
           (a==0xf0 && (uint8_t)s[i]<0x90) || (a==0xf4 && (uint8_t)s[i]>=0x90))return 0;
        while(k--)if(((uint8_t)s[i++]&0xc0)!=0x80)return 0;
    }
    return 1;
}
static uint32_t string(preparation *p,const char *value) {
    if(!value || p->status)return 0;
    if(!text(value)) {fail(p,MH3_PREPARE_INVALID);return 0;}
    for(uint32_t i=0;i<p->strings;i++)if(!strcmp(p->w->strings[i].name,value))return p->w->strings[i].offset;
    if(p->strings==p->w->string_count) {fail(p,MH3_PREPARE_RANGE);return 0;}
    uint32_t at=0;fail(p,mh3_prepare_blob(&p->mesh,value,strlen(value)+1,1,&at));
    if(!p->status)p->w->strings[p->strings++]=(mh3_prepare_string){value,at};return at;
}
static uint32_t blob(preparation *p,const void *data,size_t bytes,uint32_t alignment) {
    uint32_t at=0;if(!p->status)fail(p,mh3_prepare_blob(&p->mesh,data,bytes,alignment,&at));return at;
}
static void record(preparation *p,mh3_prepare_writer *w,unsigned table,const uint32_t *words,size_t n) {
    if(!p->status)fail(p,mh3_prepare_record(w,table,words,n));
}
static mh3_prepare_status entry(const mh3_res_model *m,mh3_res_kind kind,uint32_t id,mh3_res_entry *e,size_t minimum) {
    if(mh3_res_model_entry(m,kind,id,e)!=MH3_RES_OK || !model_fits(m,e->offset,minimum))return MH3_PREPARE_INVALID;
    uint32_t size=be32(m->container.data+e->offset);size_t parent;
    if(size<minimum || !model_fits(m,e->offset,size) ||
       mh3_res_relative(m->container,e->offset,e->offset+4,1,&parent)!=MH3_RES_OK || parent!=m->offset)return MH3_PREPARE_INVALID;
    const char *n=name(m->container,e->offset,e->offset+8,0);
    if(!n || !text(e->name) || strcmp(n,e->name) || be32(m->container.data+e->offset+12)!=id)return MH3_PREPARE_INVALID;
    return MH3_PREPARE_OK;
}
static mh3_prepare_status dictionary(const mh3_res_model *m,mh3_res_kind kind,uint32_t *count) {
    size_t at;mh3_res_status s=mh3_res_model_dictionary(m,kind,&at,count);
    if(s==MH3_RES_ABSENT) {*count=0;return MH3_PREPARE_OK;}
    return s==MH3_RES_OK && *count<65535?MH3_PREPARE_OK:MH3_PREPARE_INVALID;
}
static uint32_t layout_index(preparation *p,const mh3_pack_layout *layout,int append) {
    for(uint32_t i=0;i<p->layouts;i++)if(!memcmp(&p->w->layouts[i],layout,sizeof *layout))return i;
    if(!append) {fail(p,MH3_PREPARE_INVALID);return 0;}
    if(p->layouts==p->w->layout_count) {fail(p,MH3_PREPARE_RANGE);return 0;}
    p->w->layouts[p->layouts]=*layout;return p->layouts++;
}
/* Material source list walk. Emitted writes retain order and BP one-shot
 * masks; the same walker proves ownership at the five submitter boundaries. */
typedef struct {uint32_t bank,address,value,mask;} write;
typedef struct {write *data;size_t capacity;uint32_t count,mask,known[256];int group;uint32_t raw[256];} writes;
static int field_masks(unsigned reg,uint32_t known) {
    uint32_t masks[16];unsigned n=0;
    if(reg==0xf3) {masks[n++]=255;masks[n++]=255<<8;masks[n++]=7<<16;masks[n++]=7<<19;masks[n++]=3<<22;}
    else if(reg==0x40) {masks[n++]=1;masks[n++]=14;masks[n++]=16;}
    else if(reg==0x41) {const uint32_t v[]={1,2,4,8,16,7<<5,7<<8,1<<11,15<<12};memcpy(masks,v,sizeof v);n=9;}
    else if(reg==0x42) {masks[n++]=255;masks[n++]=256;}
    else if(reg>=0xe0 && reg<=0xe7) {masks[n++]=2047;masks[n++]=2047<<12;masks[n++]=1<<23;}
    else if(reg>=6 && reg<=14) {masks[n++]=2047;masks[n++]=2047<<11;masks[n++]=3<<22;}
    else if(reg==0x25 || reg==0x26)for(unsigned i=0;i<4;i++)masks[n++]=15u<<(4*i);
    else if(reg==0x27)for(unsigned i=0;i<8;i++)masks[n++]=7u<<(3*i);
    else if(reg>=0x28 && reg<=0x2f)for(unsigned i=0;i<2;i++) {masks[n++]=7u<<(12*i);masks[n++]=7u<<(12*i+3);masks[n++]=1u<<(12*i+6);masks[n++]=7u<<(12*i+7);}
    else if(reg>=0xc0 && reg<=0xdf) {
        if(!(reg&1))for(unsigned i=0;i<4;i++)masks[n++]=15u<<(4*i);
        else {masks[n++]=3;masks[n++]=3<<2;for(unsigned i=0;i<4;i++)masks[n++]=7u<<(4+3*i);}
        masks[n++]=3<<16;masks[n++]=1<<18;masks[n++]=1<<19;masks[n++]=3<<20;masks[n++]=3<<22;
    } else if(reg>=0xf6 && reg<=0xfd) {const uint32_t v[]={3,3<<2,31<<4,31<<9,31<<14,31<<19};memcpy(masks,v,sizeof v);n=6;}
    else if(reg>=0x10 && reg<=0x1f) {const uint32_t v[]={3,3<<2,7<<4,3<<7,15<<9,7<<13,7<<16,1<<19,1<<20};memcpy(masks,v,sizeof v);n=9;}
    else return 0;
    for(unsigned i=0;i<n;i++)if((known&masks[i]) && (known&masks[i])!=masks[i])return 0;
    return 1;
}
static int allowed(int group,unsigned bank,unsigned reg) {
    if(group<0)return 1;
    if(group==3)return bank==1 && ((reg>=0x1040 && reg<0x1048) || (reg>=0x1050 && reg<0x1058));
    if(bank)return 0;
    return group==0?(reg==0xf3 || reg==0x40 || reg==0x41 || reg==0x42):
        group==1?(reg>=0xe0 && reg<=0xe7):group==2?((reg>=6 && reg<=14)||reg==0x25||reg==0x26):
        ((reg>=0x10 && reg<=0x1f)||(reg>=0x27 && reg<=0x2f)||(reg>=0xc0 && reg<=0xdf)||(reg>=0xf6 && reg<=0xfd));
}
static mh3_prepare_status put_write(writes *w,uint32_t bank,uint32_t address,uint32_t value,uint32_t mask) {
    if(!allowed(w->group,bank,address))return MH3_PREPARE_INVALID;
    if(bank==0) {w->known[address]|=mask;w->raw[address]=value;}
    if(w->data) {if(w->count>=w->capacity)return MH3_PREPARE_RANGE;w->data[w->count]=(write){bank,address,value,mask};}
    w->count++;return MH3_PREPARE_OK;
}
static mh3_prepare_status lists(mh3_res_span list,writes *w) {
    for(size_t at=0;at<list.size;) {
        unsigned op=list.data[at++];if(!op)continue;mh3_prepare_status s;
        if(op==0x61) {
            if(!fits(list.size,at,4))return MH3_PREPARE_RANGE;
            uint32_t word=be32(list.data+at);at+=4;unsigned reg=word>>24;
            if(reg==0xfe) {if(w->group==3)return MH3_PREPARE_INVALID;w->mask=word&0xffffff;w->raw[reg]=w->mask;continue;}
            s=put_write(w,0,reg,word&0xffffff,w->mask);w->mask=0xffffff;
        } else if(op==8) {
            if(!fits(list.size,at,5))return MH3_PREPARE_RANGE;
            s=put_write(w,2,list.data[at],be32(list.data+at+1),UINT32_MAX);at+=5;
        } else if(op==0x10) {
            if(!fits(list.size,at,4))return MH3_PREPARE_RANGE;
            uint32_t count=be16(list.data+at)+1,reg=be16(list.data+at+2);at+=4;
            if(!fits(list.size,at,(size_t)count*4))return MH3_PREPARE_RANGE;
            s=MH3_PREPARE_OK;for(uint32_t i=0;i<count && !s;i++)s=put_write(w,1,reg+i,be32(list.data+at+4*i),UINT32_MAX);at+=(size_t)count*4;
        } else return MH3_PREPARE_INVALID;
        if(s)return s;
    }
    if(w->group>=0) {
        if(w->mask!=0xffffff)return MH3_PREPARE_INVALID;
        for(unsigned r=0;r<256;r++)if(w->known[r] && !field_masks(r,w->known[r]))return MH3_PREPARE_INVALID;
        if(w->group==2)for(unsigned i=0;i<3;i++) {
            uint32_t a=w->known[6+3*i]&(3<<22),b=w->known[7+3*i]&(3<<22),c=w->known[8+3*i]&(3<<22);
            if((a||b||c) && (a!=(3<<22)||b!=(3<<22)||c!=(3<<22)))return MH3_PREPARE_INVALID;
        }
    }
    return MH3_PREPARE_OK;
}
typedef struct {mh3_res_entry entry;uint32_t size;size_t pixel,tev,texture;uint32_t bindings,block_bytes[5];size_t blocks[5];} material;
static mh3_prepare_status material_open(const mh3_res_model *m,uint32_t id,material *out) {
    material x={0};mh3_prepare_status s=entry(m,MH3_RES_MATERIALS,id,&x.entry,0x44);if(s)return s;
    const uint8_t *a=m->container.data+x.entry.offset;x.size=be32(a);x.bindings=be32(a+0x2c);
    if(x.bindings>8 || a[20]>8 || a[21]>2 || !a[22] || a[22]>16 || a[23]>4 || be32(a+24)>3)return MH3_PREPARE_INVALID;
    if(mh3_res_relative(m->container,x.entry.offset,x.entry.offset+0x3c,32,&x.pixel)!=MH3_RES_OK ||
       mh3_res_relative(m->container,x.entry.offset,x.entry.offset+0x28,32,&x.tev)!=MH3_RES_OK ||
       !model_fits(m,x.tev,32))return MH3_PREPARE_INVALID;
    const uint8_t *t=m->container.data+x.tev;uint32_t tev_size=be32(t),emitted=(96+((a[22]+1)/2)*48+31)&~31u;
    if(t[12]!=a[22] || tev_size<32+emitted || !model_fits(m,x.tev,tev_size) ||
       memcmp(a+0x3c,(uint8_t[4]){0},4)==0)return MH3_PREPARE_INVALID;
    for(unsigned i=0;i<8;i++)if(t[16+i]>7 && t[16+i]!=255)return MH3_PREPARE_INVALID;
    for(unsigned i=0;i<5;i++) {
        static const uint32_t offsets[4]={0,32,160,224},sizes[4]={32,128,64,160};
        x.blocks[i]=i==4?x.tev+32:x.pixel+offsets[i];x.block_bytes[i]=i==4?emitted:sizes[i];
        if(i<4 && (x.blocks[i]<x.entry.offset || !fits(x.size,x.blocks[i]-x.entry.offset,x.block_bytes[i])))return MH3_PREPARE_RANGE;
        writes w={0};w.mask=0xffffff;w.group=(int)i;
        s=lists((mh3_res_span){m->container.data+x.blocks[i],x.block_bytes[i]},&w);if(s)return s;
    }
    if(x.bindings) {
        if(mh3_res_relative(m->container,x.entry.offset,x.entry.offset+0x30,x.bindings*52,&x.texture)!=MH3_RES_OK ||
           x.texture<x.entry.offset || !fits(x.size,x.texture-x.entry.offset,x.bindings*52))return MH3_PREPARE_RANGE;
    }
    *out=x;return MH3_PREPARE_OK;
}
typedef struct {uint32_t value,known;} reg;
typedef struct {reg bp[256],xf[96];int color[2][4][4],matrix[8];} registers;
static int field(reg r,unsigned shift,unsigned width) {
    const uint32_t bits=((1u<<width)-1)<<shift;
    return (r.known&bits)==bits?(int)(r.value>>shift&((1u<<width)-1)):-1;
}
static int bp(const registers *r,unsigned address,unsigned shift,unsigned width) {return field(r->bp[address],shift,width);}
static int xf(const registers *r,unsigned address,unsigned shift,unsigned width) {return field(r->xf[address-0x1000],shift,width);}
static void fold(registers *r,const uint8_t *data,uint32_t count) {
    memset(r,0,sizeof *r);for(unsigned b=0;b<2;b++)for(unsigned c=0;c<4;c++)for(unsigned i=0;i<4;i++)r->color[b][c][i]=INT32_MIN;
    for(unsigned i=0;i<8;i++)r->matrix[i]=-1;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *p=data+(size_t)i*16;
        unsigned bank=get32(p),address=get32(p+4);uint32_t value=get32(p+8),mask=get32(p+12);
        reg *target=bank==0?&r->bp[address]:bank==1 && address>=0x1000 && address<0x1060?&r->xf[address-0x1000]:NULL;
        if(target) {target->value=(target->value&~mask)|(value&mask);target->known|=mask;}
        if(bank==0 && address>=0xe0 && address<=0xe7 && (target->known&(1<<23))) {
            unsigned bank2=target->value>>23&1,width=bank2?8:11,index=(address-0xe0)/2;
            for(unsigned j=0;j<2;j++) {
                unsigned component=(address&1)?(j?1:2):(j?3:0),shift=j*12;int v=field(*target,shift,width);
                if(v>=0)r->color[bank2][index][component]=!bank2 && (v&1024)?v-2048:v;
                else if(mask&(((1u<<width)-1)<<shift))r->color[bank2][index][component]=INT32_MIN;
            }
        }
        for(unsigned t=0;t<8;t++)if((bank==2 && address==(t<4?0x30u:0x40u)) || (bank==1 && address==(t<4?0x1018u:0x1019u))) {
            unsigned shift=t<4?6+6*t:6*(t-4);uint32_t bits=63u<<shift;
            if((mask&bits)==bits)r->matrix[t]=(int)(value>>shift&63);else if(mask&bits)r->matrix[t]=-1;
        }
    }
}
static void values(uint8_t *out,const int *data,unsigned count,unsigned bytes) {
    uint64_t known=0;for(unsigned i=0;i<count;i++)if(data[i]>=0)known|=UINT64_C(1)<<i;
    if(bytes==8)le64(out,known);else le32(out,(uint32_t)known);
}
static size_t typed_material(const mh3_res_model *m,const material *mat,const uint8_t *source,uint32_t count,uint8_t *out) {
    const uint8_t *a=m->container.data+mat->entry.offset;unsigned stages=a[22],texgens=a[20];
    size_t constant=1152+48*stages+32*texgens,total=constant+336;memset(out,0,total);
    const int fixed=mat->size>=0x418;uint32_t srt_flags=fixed?be32(a+0x1a8):0;
    for(unsigned i=0;i<8;i++)if(fixed) {
        const uint8_t *s=a+0x1b0+i*20,*e=a+0x250+i*52;uint8_t *p=out+128+i*96;
        le32(p,srt_flags>>(4*i)&15);le32(p+4,(uint32_t)(int32_t)(int8_t)e[0]);le32(p+8,(uint32_t)(int32_t)(int8_t)e[1]);
        le32(p+12,e[2]);le32(p+16,e[3]);for(unsigned j=0;j<5;j++)le32(p+20+4*j,be32(s+4*j));
        for(unsigned j=0;j<12;j++)le32(p+40+4*j,be32(e+4+4*j));
    }
    for(unsigned i=0;i<2;i++)if(fixed) {
        const uint8_t *s=a+0x3f0+i*20;uint8_t *p=out+896+i*64;le32(p,be32(s));memcpy(p+4,s+4,8);
        for(unsigned j=0;j<2;j++) {
            uint32_t raw=be32(s+12+4*j);uint8_t *v=p+12+j*8;int select=(raw&(1<<10))!=0;
            v[0]=(uint8_t)(raw&1);v[1]=(uint8_t)(raw>>6&1);v[2]=(uint8_t)(raw>>1&1);
            v[3]=(uint8_t)(select?(raw>>7&3):0);v[4]=(uint8_t)(!select?0:(raw&(1<<9))?1:2);
            v[5]=(uint8_t)((raw>>2&15)|(raw>>7&0xf0));le32(p+28+4*j,raw);
        }
    }
    registers r;fold(&r,source,count);
    static const unsigned pixels[][3]={{0xf3,16,3},{0xf3,0,8},{0xf3,19,3},{0xf3,8,8},{0xf3,22,2},
        {0x40,0,1},{0x40,1,3},{0x40,4,1},{0x41,0,1},{0x41,1,1},{0x41,11,1},{0x41,8,3},{0x41,5,3},{0x41,12,4},
        {0x41,3,1},{0x41,4,1},{0x41,2,1},{0x42,8,1},{0x42,0,8},{0xf5,2,2},{0xf5,0,2},{0xf4,0,24}};
    int pixel[22];for(unsigned i=0;i<22;i++) {pixel[i]=bp(&r,pixels[i][0],pixels[i][1],pixels[i][2]);le32(out+1032+4*i,pixel[i]<0?0:(uint32_t)pixel[i]);}
    values(out+1024,pixel,22,8);unsigned classification=0;int known=1;
    for(unsigned i=0;i<5;i++)if(pixel[i]<0)known=0;
    if(pixel[8]<0||pixel[9]<0||pixel[10]<0)known=0;
    if(known)classification=pixel[8]||pixel[9]||pixel[10]?3:pixel[0]==7&&pixel[2]==7&&pixel[4]==0?1:2;
    le32(out+1120,classification);
    for(unsigned i=0;i<stages;i++) {
        int v[34];for(unsigned j=0;j<34;j++)v[j]=-1;uint8_t *p=out+1152+48*i;
        for(unsigned alpha=0;alpha<2;alpha++) {
            unsigned address=0xc0+2*i+alpha,base=9*alpha;
            for(unsigned j=0;j<4;j++)v[base+j]=bp(&r,address,alpha?13-3*j:12-4*j,alpha?3:4);
            int bias=bp(&r,address,16,2),op=bp(&r,address,18,1),scale=bp(&r,address,20,2);
            v[base+4]=bias<0||op<0||(bias==3&&scale<0)?-1:bias==3?8+scale*2+op:op;
            v[base+5]=bias==3?0:bias;v[base+6]=bias<0?-1:bias==3?0:scale;
            v[base+7]=bp(&r,address,19,1);v[base+8]=bp(&r,address,22,2);
        }
        unsigned shift=12*(i&1);int enabled=bp(&r,0x28+i/2,shift+6,1);
        v[18]=enabled==0?255:enabled==1?bp(&r,0x28+i/2,shift,3):-1;v[19]=bp(&r,0x28+i/2,shift+3,3);v[20]=bp(&r,0x28+i/2,shift+7,3);
        v[21]=bp(&r,0xc1+2*i,0,2);v[22]=bp(&r,0xc1+2*i,2,2);v[23]=bp(&r,0xf6+i/2,4+10*(i&1),5);v[24]=bp(&r,0xf6+i/2,9+10*(i&1),5);
        static const unsigned indirect[][2]={{0,2},{2,2},{4,3},{7,2},{9,4},{13,3},{16,3},{19,1},{20,1}};
        for(unsigned j=0;j<9;j++)v[25+j]=bp(&r,0x10+i,indirect[j][0],indirect[j][1]);
        values(p,v,34,8);for(unsigned j=0;j<34;j++)p[8+j]=(uint8_t)(v[j]<0?0:v[j]);
    }
    for(unsigned i=0;i<texgens;i++) {
        static const unsigned fields[][2]={{1,1},{2,1},{4,3},{7,5},{12,3},{15,3}};int v[10];
        for(unsigned j=0;j<6;j++)v[j]=xf(&r,0x1040+i,fields[j][0],fields[j][1]);if(v[1]>=0)v[1]=v[1]==0;
        v[6]=r.matrix[i];v[7]=xf(&r,0x1050+i,0,6);v[8]=xf(&r,0x1050+i,8,1);v[9]=xf(&r,0x1012,0,1);
        uint8_t *p=out+1152+48*stages+32*i;values(p,v,10,4);for(unsigned j=0;j<10;j++)p[4+j]=(uint8_t)(v[j]<0?0:v[j]);
    }
    uint8_t *p=out+constant;memcpy(p,"M3MC",4);le32(p+4,1);le32(p+8,336);p+=16;
    for(unsigned bank=0;bank<2;bank++)for(unsigned i=0;i<4;i++,p+=16) {
        /* Signed -1 is a valid TEV component, so its known bit is independent
         * of the unknown sentinel maintained below by the original fold. */
        uint32_t mask=0;for(unsigned j=0;j<4;j++)if(r.color[bank][i][j]!=INT32_MIN)mask|=1u<<j;
        le32(p,mask);for(unsigned j=0;j<4;j++)le16(p+4+2*j,(uint16_t)(r.color[bank][i][j]==INT32_MIN?0:r.color[bank][i][j]));
    }
    for(unsigned i=0;i<4;i++,p+=8) {int v[4];for(unsigned j=0;j<4;j++)v[j]=bp(&r,0xf6+2*i+j/2,2*(j&1),2);values(p,v,4,4);for(unsigned j=0;j<4;j++)p[4+j]=(uint8_t)(v[j]<0?0:v[j]);}
    for(unsigned i=0;i<4;i++,p+=16) {int v[4]={bp(&r,0x27,6*i+3,3),bp(&r,0x27,6*i,3),bp(&r,0x25+i/2,8*(i&1),4),bp(&r,0x25+i/2,8*(i&1)+4,4)};values(p,v,4,4);for(unsigned j=0;j<4;j++)p[4+j]=(uint8_t)(v[j]<0?0:v[j]);}
    for(unsigned i=0;i<3;i++,p+=32) {
        unsigned mask=0;for(unsigned j=0;j<6;j++) {int v=bp(&r,6+3*i+j%3,11*(j/3),11);if(v>=0) {mask|=1u<<j;if(v&1024)v-=2048;}le16(p+4+j*2,(uint16_t)(v<0 && !(mask&(1u<<j))?0:v));}
        int e[3];for(unsigned j=0;j<3;j++)e[j]=bp(&r,6+3*i+j,22,2);
        if(e[0]>=0&&e[1]>=0&&e[2]>=0) {mask|=64;le32(p+16,(uint32_t)(e[0]+4*e[1]+16*e[2]-17));}le32(p,mask);
    }
    const uint32_t header[]={0x544d334d,3,(uint32_t)total,(uint32_t)fixed,(uint32_t)(int32_t)(int8_t)a[29],(uint32_t)(int32_t)(int8_t)a[30],
        a[32],a[33],a[34],(uint32_t)(int32_t)(int8_t)a[36],(uint32_t)(int32_t)(int8_t)a[37],(uint32_t)(int32_t)(int8_t)a[38],
        srt_flags,fixed?be32(a+0x1ac):0,128,896,1024,1152,stages,1152+48*stages,texgens,(uint32_t)constant,336};
    for(unsigned i=0;i<23;i++)le32(out+4*i,header[i]);memcpy(out+92,m->container.data+mat->tev+16,8);le32(out+100,255);return total;
}
static mh3_prepare_status resource(mh3_res_span owner,const char *wanted,uint32_t magic,size_t *offset) {
    if(!fits(owner.size,0,16) || memcmp(owner.data,"bres\xfe\xff",6) || be32(owner.data+8)!=owner.size)return MH3_PREPARE_INVALID;
    size_t root=be16(owner.data+12);
    if(!fits(owner.size,root,8) || memcmp(owner.data+root,"root",4))return MH3_PREPARE_INVALID;
    uint32_t root_bytes=be32(owner.data+root+4),groups;
    if(root_bytes<8 || !fits(owner.size,root,root_bytes) || mh3_res_dictionary_count(owner,root+8,&groups)!=MH3_RES_OK || groups>32768 ||
       !fits(root+root_bytes,root+8,be32(owner.data+root+8)))return MH3_PREPARE_INVALID;
    uint32_t found=0;
    for(uint32_t i=0;i<groups;i++) {
        size_t group;uint32_t count;
        const char *group_name=name(owner,root+8,root+32+(size_t)i*16+8,0);
        const char *expected=magic==0x4d444c30?"3DModels(NW4R)":magic==0x54455830?"Textures(NW4R)":"Palettes(NW4R)";
        if(mh3_res_dictionary_data(owner,root+8,(int32_t)i,&group)!=MH3_RES_OK ||
           group<root+8 || !fits(root+root_bytes,group,8) ||
           mh3_res_dictionary_count(owner,group,&count)!=MH3_RES_OK || count>32768 ||
           !fits(root+root_bytes,group,be32(owner.data+group)) || !group_name || !text(group_name))return MH3_PREPARE_INVALID;
        for(uint32_t j=0;j<count;j++) {
            size_t at;
            if(mh3_res_dictionary_data(owner,group,(int32_t)j,&at)!=MH3_RES_OK || !fits(owner.size,at,12))return MH3_PREPARE_INVALID;
            uint32_t size=be32(owner.data+at+4);if(size<12 || !fits(owner.size,at,size))return MH3_PREPARE_INVALID;
            if(be32(owner.data+at)!=magic || strcmp(group_name,expected))continue;
            const char *n=name(owner,group,group+24+(size_t)j*16+8,0);
            if(!n || !text(n))return MH3_PREPARE_INVALID;
            if(!strcmp(n,wanted)) {found++;*offset=at;}
        }
    }
    return found==1?MH3_PREPARE_OK:MH3_PREPARE_INVALID;
}
static const mh3_prepare_texture *texture(preparation *p,const mh3_prepare_model_source *s,const char *n) {
    uint32_t lo=0,hi=p->texture_count;
    while(lo<hi) {uint32_t mid=lo+(hi-lo)/2;const mh3_prepare_texture *t=&p->textures[mid];
        int cmp=t->partition_offset<s->partition_offset?-1:t->partition_offset>s->partition_offset?1:strcmp(t->name,n);
        if(cmp<0)lo=mid+1;else hi=mid;
    }
    if(lo==p->texture_count)return NULL;const mh3_prepare_texture *t=&p->textures[lo];
    if(t->partition_offset!=s->partition_offset || strcmp(t->name,n) || t->container_bytes!=s->model.container.size)return NULL;
    size_t at;if(resource(s->model.container,n,0x54455830,&at) || at!=t->resource_offset || !fits(t->container_bytes,at,64))return NULL;
    const uint8_t *a=s->model.container.data+at;const char *original=name(s->model.container,at,at+20,0);
    size_t parent;
    if(be32(a+4)!=t->resource_bytes || be32(a+8)!=3 || !original || strcmp(n,original) ||
       mh3_res_relative(s->model.container,at,at+12,1,&parent)!=MH3_RES_OK || parent ||
       be16(a+28)!=t->width || be16(a+30)!=t->height)return NULL;
    return t;
}
static mh3_prepare_status node_link(const mh3_res_model *m,uint32_t count,size_t at,size_t field,uint32_t *out) {
    size_t target;mh3_res_status status=mh3_res_relative(m->container,at,field,1,&target);
    if(status==MH3_RES_ABSENT) {*out=UINT32_MAX;return MH3_PREPARE_OK;}
    if(status!=MH3_RES_OK)return MH3_PREPARE_INVALID;
    for(uint32_t i=0;i<count;i++) {mh3_res_entry e;if(mh3_res_model_entry(m,MH3_RES_NODES,i,&e)!=MH3_RES_OK)return MH3_PREPARE_INVALID;
        if(e.offset==target) {*out=i;return MH3_PREPARE_OK;}}
    return MH3_PREPARE_INVALID;
}
static mh3_prepare_status nodes(preparation *p,const mh3_prepare_model_source *s,const mh3_prepare_model_plan *plan,int emit) {
    const mh3_res_model *m=&s->model;
    for(uint32_t i=0;i<plan->nodes;i++) {
        p->diagnostic.kind=MH3_RES_NODES;p->diagnostic.resource_id=i;mh3_res_entry e;
        if(entry(m,MH3_RES_NODES,i,&e,0xd0))return MH3_PREPARE_INVALID;
        const uint8_t *a=m->container.data+e.offset;uint32_t links[4];
        for(unsigned j=0;j<4;j++)if(node_link(m,plan->nodes,e.offset,e.offset+0x5c+j*4,&links[j]))return MH3_PREPARE_INVALID;
        uint32_t cursor=i;
        for(uint32_t depth=0;cursor!=UINT32_MAX;depth++) {
            if(depth>=plan->nodes)return MH3_PREPARE_INVALID;
            mh3_res_entry current;if(mh3_res_model_entry(m,MH3_RES_NODES,cursor,&current)!=MH3_RES_OK ||
               node_link(m,plan->nodes,current.offset,current.offset+0x5c,&cursor))return MH3_PREPARE_INVALID;
        }
        if(emit) {
            uint32_t v[47]={s->asset_id,i,links[0]==UINT32_MAX?UINT32_MAX:plan->starts[1]+links[0],be32(a+16),be32(a+20),be32(a+24),
                links[1]==UINT32_MAX?UINT32_MAX:plan->starts[1]+links[1],0};
            for(unsigned j=0;j<24;j++)v[8+j]=be32(a+0x70+j*4);
            for(unsigned j=0;j<9;j++)v[32+j]=be32(a+32+j*4);
            for(unsigned j=0;j<6;j++)v[41+j]=be32(a+68+j*4);
            record(p,&p->mesh,1,v,47);p->cursor[1]++;
        }
    }
    return p->status;
}
/* mode0 plans/validates; mode1 emits draws without blobs; mode2 recipes. */
static mh3_prepare_status bytecodes(preparation *p,const mh3_prepare_model_source *s,mh3_prepare_model_plan *plan,int mode) {
    const mh3_res_model *m=&s->model;uint32_t entries;
    if(dictionary(m,MH3_RES_BYTECODE,&entries))return MH3_PREPARE_INVALID;
    uint32_t commands=0,weights=0,draws=0;
    for(uint32_t i=0;i<entries;i++) {
        mh3_res_entry e;if(mh3_res_model_entry(m,MH3_RES_BYTECODE,i,&e)!=MH3_RES_OK)return MH3_PREPARE_INVALID;
        int recipe=!strcmp(e.name,"NodeTree")||!strcmp(e.name,"NodeMix"),phase=!strcmp(e.name,"DrawXlu")?1:!strcmp(e.name,"DrawOpa")?0:-1;
        size_t at=e.offset;int ended=0;
        while(model_fits(m,at,1)) {
            unsigned op=m->container.data[at++];if(op==1) {ended=1;break;}if(!op)continue;
            uint32_t v[6]={op,0,0,0,0,0};
            if(op==2 || op==5 || op==6) {
                if(!model_fits(m,at,4))return MH3_PREPARE_RANGE;
                v[1]=be16(m->container.data+at);v[2]=be16(m->container.data+at+2);at+=4;
                if((op==2 && v[1]>=plan->nodes)||(op==5 && v[2]>=plan->nodes))return MH3_PREPARE_INVALID;
            } else if(op==3) {
                if(!model_fits(m,at,3))return MH3_PREPARE_RANGE;
                v[1]=be16(m->container.data+at);v[3]=weights;v[4]=m->container.data[at+2];at+=3;
                if(!model_fits(m,at,(size_t)v[4]*6))return MH3_PREPARE_RANGE;
                if(recipe && mode==2)for(uint32_t j=0;j<v[4];j++) {
                    uint8_t *out=p->w->scratch+16+(size_t)plan->commands*24+(size_t)(weights+j)*8;
                    le32(out,be16(m->container.data+at+j*6));le32(out+4,be32(m->container.data+at+j*6+2));
                }
                at+=(size_t)v[4]*6;if(recipe) {
                    if(v[4]>MH3_PREPARE_MAX_METADATA/8-weights)return MH3_PREPARE_RANGE;
                    weights+=v[4];
                }
            } else if(op==4) {
                if(recipe || phase<0 || !model_fits(m,at,7))return MH3_PREPARE_INVALID;
                unsigned mat=be16(m->container.data+at),shape=be16(m->container.data+at+2),node=be16(m->container.data+at+4),priority=m->container.data[at+6];at+=7;
                if(mat>=plan->materials || shape>=plan->shapes || node>=plan->nodes)return MH3_PREPARE_INVALID;
                if(mode==1) {
                    uint32_t d[8]={plan->starts[3]+shape,plan->starts[4]+mat,plan->starts[1]+node,(uint32_t)phase,priority,draws,(s->asset_id<<16)|(shape+1),7};
                    record(p,&p->mesh,6,d,8);p->cursor[6]++;
                }
                if(draws==MH3_PREPARE_MAX_METADATA/32)return MH3_PREPARE_RANGE;draws++;
            } else return MH3_PREPARE_INVALID;
            if(recipe) {
                if(mode==2)for(unsigned j=0;j<6;j++)le32(p->w->scratch+16+(size_t)commands*24+j*4,v[j]);
                if(commands==MH3_PREPARE_MAX_METADATA/24)return MH3_PREPARE_RANGE;commands++;
            }
        }
        if(!ended)return MH3_PREPARE_INVALID;
    }
    if(!mode) {plan->commands=commands;plan->weights=weights;plan->draws=draws;}
    else if(commands!=plan->commands || weights!=plan->weights || draws!=plan->draws)return MH3_PREPARE_INVALID;
    return p->status;
}
static mh3_prepare_status materials(preparation *p,const mh3_prepare_model_source *s,const mh3_prepare_model_plan *plan) {
    const mh3_res_model *m=&s->model;
    for(uint32_t i=0;i<plan->materials;i++) {
        p->diagnostic.kind=MH3_RES_MATERIALS;p->diagnostic.resource_id=i;material mat;
        mh3_prepare_status status=material_open(m,i,&mat);if(status)return status;
        const uint8_t *a=m->container.data+mat.entry.offset;size_t typed_bytes=1488+48*a[22]+32*a[20];
        if(p->w->scratch_bytes<typed_bytes)return MH3_PREPARE_RANGE;
        writes w={0};w.group=-1;w.mask=0xffffff;w.data=(write *)p->w->scratch;w.capacity=(p->w->scratch_bytes-typed_bytes)/16;
        status=lists((mh3_res_span){m->container.data+mat.pixel,mat.entry.offset+mat.size-mat.pixel},&w);if(status)return status;
        uint32_t tev_bytes=be32(m->container.data+mat.tev);
        status=lists((mh3_res_span){m->container.data+mat.tev+32,tev_bytes-32},&w);if(status)return status;
        for(uint32_t j=0;j<w.count;j++) {write value=w.data[j];uint8_t *out=p->w->scratch+(size_t)j*16;
            le32(out,value.bank);le32(out+4,value.address);le32(out+8,value.value);le32(out+12,value.mask);}
        uint32_t write_offset=blob(p,p->w->scratch,(size_t)w.count*16,4),mat_name=string(p,mat.entry.name);
        uint8_t *typed=p->w->scratch+(size_t)w.count*16;
        size_t size=typed_material(m,&mat,p->w->scratch,w.count,typed);
        uint32_t typed_offset=blob(p,typed,size,16);
        writes raw={0};raw.group=-1;raw.mask=0xffffff;raw.raw[0xfe]=0xffffff;
        status=lists((mh3_res_span){m->container.data+mat.pixel,32},&raw);if(status)return status;
        uint32_t index=p->cursor[4],v[23]={(s->asset_id<<16)|(i+1),s->asset_id,i,(be32(a+16)>>31)|2,mat_name,be32(a+24),
            a[20],a[21],a[22],a[23],a[28]!=0,raw.raw[0xf3],raw.raw[0x40],raw.raw[0x41],raw.raw[0xfe],write_offset,w.count,
            p->cursor[5],mat.bindings,(uint32_t)mat.tev,0,typed_offset,(uint32_t)size};
        record(p,&p->mesh,4,v,23);p->cursor[4]++;
        for(uint32_t j=0;j<mat.bindings;j++) {
            const size_t at=mat.texture+j*52;const uint8_t *t=m->container.data+at;
            const char *n=name(m->container,at,at,0),*pal=name(m->container,at,at+4,1);
            if(!n || (be32(t+4) && !pal))return MH3_PREPARE_INVALID;
            const mh3_prepare_texture *asset=texture(p,s,n);if(!asset)return MH3_PREPARE_INVALID;
            if(pal) {
                size_t offset,parent;if(resource(m->container,pal,0x504c5430,&offset) || !fits(m->container.size,offset,64))return MH3_PREPARE_INVALID;
                const char *original=name(m->container,offset,offset+20,0);
                if(be32(m->container.data+offset+4)<64 || be32(m->container.data+offset+8)!=3 || !original || strcmp(original,pal) ||
                   mh3_res_relative(m->container,offset,offset+12,1,&parent)!=MH3_RES_OK || parent)return MH3_PREPARE_INVALID;
            }
            uint32_t bind[16]={index,asset->id,be32(t+16),be32(t+20),be32(t+24),be32(t+28),be32(t+32),be32(t+36),
                be32(t+40),be32(t+44),t[48]!=0,t[49]!=0,0,0,0,0};
            bind[12]=string(p,n);bind[13]=string(p,pal);record(p,&p->mesh,5,bind,16);p->cursor[5]++;
        }
        if(p->status)return p->status;
    }
    return MH3_PREPARE_OK;
}
static void face(uint16_t *faces,uint32_t *count,const uint16_t *corners,unsigned a,unsigned b,unsigned c) {
    if(faces) {faces[3u*(*count)]=corners[a];faces[3u*(*count)+1]=corners[b];faces[3u*(*count)+2]=corners[c];}(*count)++;
}
static uint32_t primitive_count(const mh3_res_shape *shape,uint16_t *faces,uint16_t *corners,uint32_t capacity) {
    uint32_t cp[256]={0},primitives=0,triangles=0;
    for(unsigned list_id=0;list_id<2;list_id++) {
        mh3_res_span list=list_id?shape->primitives.commands:shape->setup.commands;
        for(size_t at=0;at<list.size;) {
            unsigned op=list.data[at++];if(!op)continue;
            if(op==8) {if(!fits(list.size,at,5))return UINT32_MAX;cp[list.data[at]]=be32(list.data+at+1);at+=5;}
            else if(op==0x61 || op==0x20 || op==0x28 || op==0x30 || op==0x38) {if(!fits(list.size,at,4))return UINT32_MAX;at+=4;}
            else if(op==0x10) {if(!fits(list.size,at,4))return UINT32_MAX;size_t n=(size_t)(be16(list.data+at)+1)*4;
                if(!fits(list.size,at+4,n))return UINT32_MAX;at+=4+n;}
            else {
                if(!list_id || !fits(list.size,at,2))return UINT32_MAX;
                uint32_t count=be16(list.data+at),vat=op&7;at+=2;
                uint32_t lo=cp[0x50],hi=cp[0x60],a=cp[0x70+vat],b=cp[0x80+vat],c=cp[0x90+vat],stride=0;
                for(unsigned i=0;i<9;i++)stride+=(lo>>i)&1;
                const unsigned types[12]={(a>>1)&7,(a>>10)&7,(a>>14)&7,(a>>18)&7,(a>>22)&7,
                    (b>>1)&7,(b>>10)&7,(b>>19)&7,(b>>28)&7,(c>>6)&7,(c>>15)&7,(c>>24)&7};
                const unsigned components[12]={2+(a&1),3,4,4,1+(a>>21&1),1+(b&1),1+(b>>9&1),1+(b>>18&1),1+(b>>27&1),1+(c>>5&1),1+(c>>14&1),1+(c>>23&1)};
                for(unsigned i=0;i<12;i++) {
                    unsigned mode=i<4?(lo>>(9+2*i))&3:(hi>>(2*(i-4)))&3;
                    if(mode>=2)stride+=mode-1;
                    else if(mode) {static const unsigned numbers[5]={1,1,2,2,4},colors[6]={2,3,4,2,3,4};
                        if(types[i]>=(i==2||i==3?6u:5u))return UINT32_MAX;
                        stride+=i==2||i==3?colors[types[i]]:numbers[types[i]]*components[i];}
                }
                if(!fits(list.size,at,(size_t)count*stride))return UINT32_MAX;
                if(faces) {
                    unsigned mode=(lo>>9)&3,position=0;for(unsigned j=0;j<9;j++)position+=(lo>>j)&1;
                    if(mode<2 || count>capacity)return UINT32_MAX;
                    for(uint32_t j=0;j<count;j++)corners[j]=mode==2?list.data[at+(size_t)j*stride+position]:be16(list.data+at+(size_t)j*stride+position);
                    unsigned kind=op&0xf8;
                    uint32_t add=kind==0x90?count/3:kind==0x80?(count/4)*2+(count%4==3):count>2?count-2:0;
                    if(triangles>shape->triangle_count || add>shape->triangle_count-triangles)return UINT32_MAX;
                    if(kind==0x90)for(unsigned j=0;j+2<count;j+=3)face(faces,&triangles,corners,j,j+1,j+2);
                    else if(kind==0x98)for(unsigned j=0;j+2<count;j++)face(faces,&triangles,corners,j+(j&1),j+1-(j&1),j+2);
                    else if(kind==0xa0)for(unsigned j=1;j+1<count;j++)face(faces,&triangles,corners,0,j,j+1);
                    else {for(unsigned j=0;j+3<count;j+=4) {face(faces,&triangles,corners,j,j+1,j+2);face(faces,&triangles,corners,j+2,j+3,j);}
                        if(count%4==3)face(faces,&triangles,corners,count-3,count-2,count-1);}
                }
                at+=(size_t)count*stride;primitives++;
            }
        }
    }
    return faces && triangles!=shape->triangle_count?UINT32_MAX:primitives;
}
static uint32_t find(uint32_t *parents,uint32_t id) {
    if(parents[id]==UINT32_MAX)parents[id]=id;
    while(parents[id]!=id) {parents[id]=parents[parents[id]];id=parents[id];}return id;
}
static void point(const uint8_t *data,const mh3_native_vertex_source *array,unsigned index,double out[3]) {
    const uint8_t *p=data+array->payload_address+(size_t)index*array->stride;
    for(unsigned i=0;i<3;i++) {
        if(i>=array->components) {out[i]=0;continue;}
        if(array->format==4) {uint32_t bits=be32(p);float f;memcpy(&f,&bits,4);out[i]=f;p+=4;}
        else {int value=array->format==0?*p:array->format==1?(*p<128?*p:(int)*p-256):array->format==2?be16(p):be16(p)<32768?be16(p):(int)be16(p)-65536;
            out[i]=ldexp((double)value,-(int)array->fraction);p+=array->format<2?1:2;}
    }
}
static int selected(const mh3_prepare_model_source *s,uint32_t id) {
    for(uint32_t i=0;i<s->component_shape_count;i++)if(s->component_shapes[i]==id)return 1;return 0;
}
static mh3_prepare_status components(preparation *p,const mh3_prepare_model_source *s,uint32_t shape_id,const mh3_prepared_corners *prepared,
    int emit,uint32_t *count,uint32_t *template_offset) {
    *count=0;*template_offset=0;if(!selected(s,shape_id))return MH3_PREPARE_OK;
    if(prepared->matrix_mask)return MH3_PREPARE_INVALID;
    mh3_res_shape shape;mh3_res_entry e;mh3_native_vertex_source array;
    if(mh3_res_shape_open(&s->model,shape_id,&shape)!=MH3_RES_OK || shape.matrix_id<0 ||
       mh3_res_shape_array(&s->model,&shape,0,&e)!=MH3_RES_OK || !fits(s->model.container.size,e.offset,32))return MH3_PREPARE_INVALID;
    const uint8_t *data=s->model.container.data+e.offset;uint32_t resource_bytes=be32(data);
    if(!fits(s->model.container.size,e.offset,resource_bytes) || mh3_vertex_source_read((mh3_res_span){data,resource_bytes},0,MH3_VERTEX_POSITION,&array)!=MH3_RES_OK)return MH3_PREPARE_INVALID;
    uint64_t vertex_bytes=(uint64_t)array.count*8,face_bytes=(uint64_t)shape.triangle_count*6,root_at=(vertex_bytes+face_bytes+3)&~UINT64_C(3);
    uint64_t original_at=root_at+(uint64_t)shape.triangle_count*4,template_at=original_at+face_bytes,total=template_at+face_bytes;
    if(total>p->w->scratch_bytes)return MH3_PREPARE_RANGE;
    uint32_t *parents=(uint32_t *)p->w->scratch,*root_ids=parents+array.count,*component_roots=(uint32_t *)(p->w->scratch+(size_t)root_at);
    uint16_t *faces=(uint16_t *)(p->w->scratch+(size_t)vertex_bytes);uint8_t *original=p->w->scratch+(size_t)original_at,*output=p->w->scratch+(size_t)template_at;
    memset(parents,255,(size_t)vertex_bytes);
    if(primitive_count(&shape,faces,p->w->shape.corners,p->w->shape.corner_count)==UINT32_MAX)return MH3_PREPARE_INVALID;
    for(uint32_t i=0;i<shape.triangle_count;i++) {
        uint16_t *f=faces+3*i;if(f[0]>=array.count || f[1]>=array.count || f[2]>=array.count || f[0]==f[1] || f[1]==f[2] || f[0]==f[2])return MH3_PREPARE_INVALID;
        uint32_t root=find(parents,f[0]);parents[find(parents,f[1])]=root;parents[find(parents,f[2])]=root;
    }
    for(uint32_t i=0;i<shape.triangle_count;i++) {uint32_t root=find(parents,faces[3*i]);
        if(root_ids[root]==UINT32_MAX) {root_ids[root]=(*count)++;component_roots[*count-1]=root;}}
    if(!emit)return MH3_PREPARE_OK;
    for(uint64_t at=0;at<face_bytes;) {size_t n=face_bytes-at>4096?4096:(size_t)(face_bytes-at);
        if(p->w->indices.read(p->w->indices.context,at,original+(size_t)at,n))return MH3_PREPARE_IO;at+=n;}
    uint32_t written=0;
    for(uint32_t c=0;c<*count;c++) {
        const uint32_t root=component_roots[c];uint32_t points=0,first=written,triangles=0;double lo[3]={0},hi[3]={0},sum[3]={0},origin[3]={0},normal[3]={0},norm=0;
        for(uint32_t i=0;i<array.count;i++)if(parents[i]!=UINT32_MAX && find(parents,i)==root) {
            double v[3];point(data,&array,i,v);
            for(unsigned j=0;j<3;j++) {if(!points)lo[j]=hi[j]=origin[j]=v[j];else {if(v[j]<lo[j])lo[j]=v[j];if(v[j]>hi[j])hi[j]=v[j];}sum[j]+=v[j];}points++;
        }
        for(uint32_t i=0;i<shape.triangle_count;i++)if(find(parents,faces[3*i])==root) {
            memcpy(output+(size_t)written*2,original+(size_t)i*6,6);written+=3;triangles++;
            double a[3],b[3],d[3],u[3],v[3],n[3];point(data,&array,faces[3*i],a);point(data,&array,faces[3*i+1],b);point(data,&array,faces[3*i+2],d);
            for(unsigned j=0;j<3;j++) {u[j]=b[j]-a[j];v[j]=d[j]-a[j];}
            n[0]=u[1]*v[2]-u[2]*v[1];n[1]=u[2]*v[0]-u[0]*v[2];n[2]=u[0]*v[1]-u[1]*v[0];
            double length=sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(length>norm) {norm=length;memcpy(normal,n,sizeof n);}
        }
        double planar=0,diagonal=0;
        if(norm)for(uint32_t i=0;i<array.count;i++)if(parents[i]!=UINT32_MAX && find(parents,i)==root) {
            double v[3];point(data,&array,i,v);double d=fabs(normal[0]*(v[0]-origin[0])+normal[1]*(v[1]-origin[1])+normal[2]*(v[2]-origin[2]))/norm;if(d>planar)planar=d;
        }
        for(unsigned j=0;j<3;j++)diagonal+=(hi[j]-lo[j])*(hi[j]-lo[j]);diagonal=sqrt(diagonal);
        uint32_t v[13]={p->cursor[3],first,written-first,(uint32_t)(points==4 && triangles==2 && array.components==3 && norm && planar<=fmax(1e-4,diagonal*1e-5))};
        for(unsigned j=0;j<3;j++) {v[4+j]=mh3_prepare_float_bits((float)lo[j]);v[7+j]=mh3_prepare_float_bits((float)hi[j]);v[10+j]=mh3_prepare_float_bits((float)(sum[j]/points));}
        record(p,&p->mesh,7,v,13);p->cursor[7]++;if(p->status)return p->status;
    }
    if(written!=prepared->index_count)return MH3_PREPARE_INVALID;
    *template_offset=blob(p,output,(size_t)written*2,4);return p->status;
}
static mh3_prepare_status corners(preparation *p,const mh3_prepare_model_source *s,mh3_prepare_model_plan *plan,int emit) {
    for(uint32_t i=0;i<plan->shapes;i++) {
        p->diagnostic.kind=MH3_RES_SHAPES;p->diagnostic.resource_id=i;mh3_prepared_corners result;size_t error=0;
        mh3_corners_status status=mh3_prepare_shape(&s->model,i,&p->w->shape,&result,&error);p->diagnostic.offset=error;
        if(status)return status==MH3_CORNERS_RANGE?MH3_PREPARE_RANGE:status==MH3_CORNERS_IO?MH3_PREPARE_IO:status==MH3_CORNERS_ROLLBACK?MH3_PREPARE_ROLLBACK:MH3_PREPARE_INVALID;
        uint32_t layout=layout_index(p,&result.layout,!emit);if(p->status)return p->status;
        if(!emit) {uint32_t count,offset;mh3_prepare_status st=components(p,s,i,&result,0,&count,&offset);if(st)return st;
            if(count>UINT32_MAX-plan->components)return MH3_PREPARE_RANGE;plan->components+=count;continue;}
        mh3_res_shape source;if(mh3_res_shape_open(&s->model,i,&source)!=MH3_RES_OK)return MH3_PREPARE_INVALID;
        uint32_t vertex=0,index=0,bytes=result.vertex_count*result.layout.stride;
        fail(p,mh3_prepare_gpu(&p->mesh,p->w->shape.vertices,bytes,64,&vertex));
        size_t chunk=p->w->scratch_bytes<4096?p->w->scratch_bytes:4096;if(!chunk)return MH3_PREPARE_RANGE;
        for(uint32_t at=0;at<result.index_count*2 && !p->status;) {
            size_t n=result.index_count*2-at;if(n>chunk)n=chunk;
            if(p->w->indices.read(p->w->indices.context,at,p->w->scratch,n))return MH3_PREPARE_IO;
            uint32_t position=0;fail(p,mh3_prepare_gpu(&p->mesh,p->w->scratch,n,at?1:64,&position));if(!at)index=position;at+=(uint32_t)n;
        }
        uint32_t component_first=p->cursor[7],component_count,template_offset;
        mh3_prepare_status st=components(p,s,i,&result,1,&component_count,&template_offset);if(st)return st;
        uint32_t palette=0;
        if(result.matrix_mask) {size_t size;status=mh3_prepare_palettes(&result,&p->w->shape,p->w->scratch,p->w->scratch_bytes,&size);
            if(status)return MH3_PREPARE_RANGE;palette=blob(p,p->w->scratch,size,4);}
        uint32_t first_node=UINT32_MAX;
        for(uint32_t j=0;j<plan->draws;j++) {const uint8_t *draw=p->mesh.data+p->mesh.offsets[6]+(size_t)(plan->starts[6]+j)*32;
            if(get32(draw)==p->cursor[3]) {first_node=get32(draw+8)-plan->starts[1];break;}}
        uint32_t v[32]={(s->asset_id<<16)|(i+1),s->asset_id,i,1|(component_count?2u:0u)|(result.matrix_mask?8u:0u),layout,result.vertex_count,result.index_count,
            vertex,bytes,index,result.index_count*2,(uint32_t)source.matrix_id,component_first,component_count,template_offset,component_count?result.index_count:0};
        for(unsigned j=0;j<3;j++) {v[16+j]=mh3_prepare_float_bits(result.bounds_min[j]);v[19+j]=mh3_prepare_float_bits(result.bounds_max[j]);v[22+j]=mh3_prepare_float_bits(result.center[j]);}
        v[25]=source.vertex_count;
        v[26]=primitive_count(&source,NULL,NULL,0);if(v[26]==UINT32_MAX)return MH3_PREPARE_INVALID;
        v[27]=(uint32_t)source.entry.offset+s->container_offset;v[28]=(uint32_t)source.primitives.offset+s->container_offset;
        v[29]=source.primitives.command_size;v[30]=first_node;v[31]=palette;record(p,&p->mesh,3,v,32);p->cursor[3]++;
        if(p->status)return p->status;
    }
    return MH3_PREPARE_OK;
}
static mh3_prepare_status model_info(const mh3_res_model *m,mh3_prepare_model_plan *plan) {
    const size_t at=m->offset+0x4c;size_t parent;
    if(!model_fits(m,at,64) || be32(m->container.data+at)<64 || !model_fits(m,at,be32(m->container.data+at)) ||
       mh3_res_relative(m->container,at,at+4,1,&parent)!=MH3_RES_OK || parent!=m->offset)return MH3_PREPARE_INVALID;
    plan->info=at;mh3_res_status s=mh3_res_relative(m->container,at,at+0x24,4,&plan->mapping);
    if(s==MH3_RES_ABSENT) {plan->mapping=0;plan->matrices=0;return MH3_PREPARE_OK;}
    if(s!=MH3_RES_OK || !model_fits(m,plan->mapping,4))return MH3_PREPARE_INVALID;
    plan->matrices=be32(m->container.data+plan->mapping);
    if((uint64_t)plan->matrices*4>m->size || !model_fits(m,plan->mapping+4,(size_t)plan->matrices*4))return MH3_PREPARE_RANGE;
    for(uint32_t i=0;i<plan->matrices;i++) {uint32_t node=be32(m->container.data+plan->mapping+4+i*4);
        if(node!=UINT32_MAX && node>=plan->nodes)return MH3_PREPARE_INVALID;}
    return MH3_PREPARE_OK;
}
static mh3_prepare_status recipe(preparation *p,const mh3_prepare_model_source *s,mh3_prepare_model_plan *plan,uint32_t *offset,uint32_t *size) {
    const uint64_t bytes=16+(uint64_t)plan->commands*24+(uint64_t)plan->weights*8+(uint64_t)plan->matrices*4;
    if(bytes>p->w->scratch_bytes)return MH3_PREPARE_RANGE;
    memset(p->w->scratch,0,(size_t)bytes);le32(p->w->scratch,plan->commands);le32(p->w->scratch+4,plan->weights);le32(p->w->scratch+8,plan->matrices);
    mh3_prepare_status status=bytecodes(p,s,plan,2);if(status)return status;
    for(uint32_t i=0;i<plan->matrices;i++)le32(p->w->scratch+16+(size_t)plan->commands*24+(size_t)plan->weights*8+i*4,
        be32(s->model.container.data+plan->mapping+4+i*4));
    *offset=blob(p,p->w->scratch,(size_t)bytes,4);*size=(uint32_t)bytes;return p->status;
}
static mh3_prepare_status emit_model(preparation *p,const mh3_prepare_model_source *s,mh3_prepare_model_plan *plan) {
    uint32_t recipe_at,size;mh3_prepare_status status=recipe(p,s,plan,&recipe_at,&size);if(status)return status;
    const char *n=name(s->model.container,s->model.offset,s->model.offset+0x48,0);if(!n)return MH3_PREPARE_INVALID;
    uint32_t v[32]={s->asset_id,string(p,n),string(p,s->file),(uint32_t)s->model.offset,plan->starts[1],plan->nodes,plan->starts[3],plan->shapes,
        plan->starts[4],plan->materials,plan->starts[6],plan->draws,recipe_at,size,11,1};
    const uint8_t *info=s->model.container.data+plan->info;
    v[16]=be32(info+8);v[17]=be32(info+12);v[18]=be32(info+28);v[19]=info[32]!=0;v[20]=info[33]!=0;v[21]=info[35];v[22]=info[34]!=0;
    for(unsigned i=0;i<6;i++)v[23+i]=be32(info+40+i*4);v[29]=be32(info+16);v[30]=be32(info+20);
    record(p,&p->mesh,0,v,32);p->cursor[0]++;return p->status;
}
static mh3_prepare_status identity(preparation *p,const mh3_prepare_model_source *sources,uint32_t count) {
    for(uint32_t i=0;i<count;i++) {
        const mh3_prepare_model_source *s=&sources[i];const mh3_prepare_model_plan *plan=&p->w->plans[i];
        uint32_t shapes=p->identity.written[1],materials_first=p->identity.written[2];
        for(unsigned kind=0;kind<2;kind++)for(uint32_t j=0;j<(kind?plan->materials:plan->shapes);j++) {
            size_t offset;uint32_t bytes;
            if(kind) {
                material mat;mh3_prepare_status status=material_open(&s->model,j,&mat);if(status)return status;
                offset=mat.entry.offset;bytes=mat.size;
                for(unsigned k=0;k<5;k++) {uint32_t block[6]={p->identity.written[2],k,(uint32_t)mat.blocks[k],mat.block_bytes[k],1,0};record(p,&p->identity,3,block,6);}
            } else {mh3_res_shape shape;if(mh3_res_shape_open(&s->model,j,&shape)!=MH3_RES_OK)return MH3_PREPARE_INVALID;offset=shape.entry.offset;bytes=shape.size;}
            uint32_t row[6]={i,j,(uint32_t)offset,bytes,(s->asset_id<<16)|(j+1),plan->starts[kind?4:3]+j};record(p,&p->identity,kind?2:1,row,6);
        }
        uint32_t row[16]={(uint32_t)s->partition_offset,(uint32_t)(s->partition_offset>>32),(uint32_t)s->model.container.size,
            (uint32_t)s->model.offset,(uint32_t)s->model.size,s->asset_id,i,11,0,shapes,plan->shapes,materials_first,plan->materials,0,0,0};
        record(p,&p->identity,0,row,16);if(p->status)return p->status;
    }
    return MH3_PREPARE_OK;
}
typedef struct {const void *data;size_t bytes;} region;
static int overlap(region a,region b) {
    uintptr_t x=(uintptr_t)a.data,y=(uintptr_t)b.data;
    return a.bytes && b.bytes && x<y+b.bytes && y<x+a.bytes;
}
static int valid_region(region a) {return !a.bytes || (a.data && a.bytes<=UINTPTR_MAX-(uintptr_t)a.data);}
static mh3_prepare_status preflight(const mh3_prepare_model_source *s,uint32_t count,const mh3_prepare_texture *textures,uint32_t texture_count,
    const mh3_prepare_model_workspace *w,mh3_prepared_model_pack *out,mh3_prepare_model_diagnostic *diagnostic) {
    if(!s || !count || count>MAX_MODELS || !w || !out || (!textures && texture_count) || texture_count>MAX_TEXTURES ||
       !w->metadata || !w->identities || !w->scratch || !w->layouts || !w->strings || !w->plans || !w->indices.read || !w->gpu.write || !w->gpu.truncate ||
       !w->shape.indices.write || !w->shape.indices.truncate)return MH3_PREPARE_INVALID;
    if(count>w->plan_count || w->plan_count>MAX_MODELS || !w->layout_count || w->layout_count>MAX_STRINGS ||
       !w->string_count || w->string_count>MAX_STRINGS || w->metadata_bytes>MH3_PREPARE_MAX_METADATA || w->identity_bytes>MH3_PREPARE_MAX_IDENTITY ||
       w->scratch_bytes>MH3_PREPARE_MAX_METADATA || !w->scratch_bytes || (uintptr_t)w->scratch%_Alignof(write) ||
       w->shape.vertex_bytes>MH3_PREPARE_MAX_METADATA || w->shape.tree_count>65536 || w->shape.corner_count>65536 ||
       w->shape.batch_count>(MH3_PREPARE_MAX_METADATA-16)/96)return MH3_PREPARE_RANGE;
    for(uint32_t i=0;i<count;i++)if(s[i].component_shape_count>=65535)return MH3_PREPARE_INVALID;
    const region mutable[]={{w->metadata,w->metadata_bytes},{w->identities,w->identity_bytes},{w->scratch,w->scratch_bytes},
        {w->layouts,(size_t)w->layout_count*sizeof *w->layouts},{w->strings,(size_t)w->string_count*sizeof *w->strings},{w->plans,(size_t)w->plan_count*sizeof *w->plans},
        {w->shape.vertices,w->shape.vertex_bytes},{w->shape.tree,(size_t)w->shape.tree_count*sizeof *w->shape.tree},{w->shape.corners,(size_t)w->shape.corner_count*sizeof *w->shape.corners},
        {w->shape.batches,(size_t)w->shape.batch_count*sizeof *w->shape.batches},{out,sizeof *out},{diagnostic,diagnostic?sizeof *diagnostic:0}};
    const region inputs[]={{s,(size_t)count*sizeof *s},{textures,(size_t)texture_count*sizeof *textures},{w,sizeof *w}};
    for(unsigned i=0;i<sizeof mutable/sizeof *mutable;i++) {
        if(!valid_region(mutable[i]))return MH3_PREPARE_INVALID;
        for(unsigned j=0;j<i;j++)if(overlap(mutable[i],mutable[j]))return MH3_PREPARE_INVALID;
        for(unsigned j=0;j<sizeof inputs/sizeof *inputs;j++)if(!valid_region(inputs[j]) || overlap(mutable[i],inputs[j]))return MH3_PREPARE_INVALID;
        for(uint32_t j=0;j<count;j++)if(!valid_region((region){s[j].model.container.data,s[j].model.container.size}) ||
            overlap(mutable[i],(region){s[j].model.container.data,s[j].model.container.size}) ||
            !valid_region((region){s[j].component_shapes,(size_t)s[j].component_shape_count*2}) ||
            overlap(mutable[i],(region){s[j].component_shapes,(size_t)s[j].component_shape_count*2}))return MH3_PREPARE_INVALID;
    }
    uint8_t ids[MAX_TEXTURES/8]={0};
    for(uint32_t i=0;i<texture_count;i++) {
        const mh3_prepare_texture *t=&textures[i];
        if(!text(t->name) || !t->id || t->id>MAX_TEXTURES || (ids[(t->id-1)/8]&(1u<<((t->id-1)%8))) ||
           !t->container_bytes || t->container_bytes>MH3_PREPARE_MAX_METADATA || t->partition_offset>UINT64_MAX-t->container_bytes ||
           !fits(t->container_bytes,t->resource_offset,t->resource_bytes) || t->resource_bytes<64 || !t->width || !t->height || t->width>2048 || t->height>2048)return MH3_PREPARE_INVALID;
        ids[(t->id-1)/8]|=(uint8_t)(1u<<((t->id-1)%8));
        if(i && (textures[i-1].partition_offset>t->partition_offset || (textures[i-1].partition_offset==t->partition_offset &&
            (textures[i-1].container_bytes!=t->container_bytes || strcmp(textures[i-1].name,t->name)>=0))))return MH3_PREPARE_INVALID;
    }
    for(uint32_t i=0;i<count;i++) {
        if(!text(s[i].file) || !s[i].asset_id || s[i].asset_id>=65536 || s[i].model.container.size>MH3_PREPARE_MAX_METADATA ||
           s[i].container_offset>UINT32_MAX-s[i].model.container.size || s[i].partition_offset<s[i].container_offset ||
           s[i].partition_offset>UINT64_MAX-s[i].model.container.size ||
           (s[i].component_shape_count && !s[i].component_shapes))return MH3_PREPARE_INVALID;
        if(i && (strcmp(s[0].file,s[i].file) || s[i].partition_offset-s[i].container_offset!=s[0].partition_offset-s[0].container_offset ||
            s[i].partition_offset<s[i-1].partition_offset || (s[i].partition_offset==s[i-1].partition_offset && s[i].model.offset<=s[i-1].model.offset) ||
            (s[i].partition_offset!=s[i-1].partition_offset && s[i].partition_offset<s[i-1].partition_offset+s[i-1].model.container.size)))return MH3_PREPARE_INVALID;
        for(uint32_t j=0;j<i;j++)if(s[i].asset_id==s[j].asset_id)return MH3_PREPARE_INVALID;
        for(uint32_t j=1;j<s[i].component_shape_count;j++)if(s[i].component_shapes[j]<=s[i].component_shapes[j-1])return MH3_PREPARE_INVALID;
    }
    /* Names are borrowed throughout both passes and string interning. They
     * may be in the immutable source, but cannot alias any mutated workspace. */
    for(unsigned i=0;i<sizeof mutable/sizeof *mutable;i++) {
        for(uint32_t j=0;j<count;j++)if(overlap(mutable[i],(region){s[j].file,strlen(s[j].file)+1}))return MH3_PREPARE_INVALID;
        for(uint32_t j=0;j<texture_count;j++)if(overlap(mutable[i],(region){textures[j].name,strlen(textures[j].name)+1}))return MH3_PREPARE_INVALID;
    }
    return MH3_PREPARE_OK;
}
mh3_prepare_status mh3_prepare_model_pack(const mh3_prepare_model_source *sources,uint32_t count,const mh3_prepare_texture *textures,
    uint32_t texture_count,const mh3_prepare_model_workspace *work,mh3_prepared_model_pack *out,mh3_prepare_model_diagnostic *diagnostic) {
    mh3_prepare_status status=preflight(sources,count,textures,texture_count,work,out,diagnostic);if(status)return status;
    preparation p={0};p.w=work;p.textures=textures;p.texture_count=texture_count;uint32_t counts[8]={count,0,0,0,0,0,0,0};
    for(uint32_t i=0;i<count && !p.status;i++) {
        const mh3_prepare_model_source *s=&sources[i];mh3_prepare_model_plan *plan=&work->plans[i];memset(plan,0,sizeof *plan);
        p.diagnostic=(mh3_prepare_model_diagnostic){s->asset_id,MH3_RES_BYTECODE,0,s->model.offset};
        mh3_res_model actual;size_t parent,owned;const char *n=name(s->model.container,s->model.offset,s->model.offset+0x48,0);
        if(mh3_res_model_open(s->model.container,s->model.offset,&actual)!=MH3_RES_OK || actual.version!=11 ||
           actual.version!=s->model.version || actual.size!=s->model.size ||
           !n || resource(actual.container,n,0x4d444c30,&owned) || owned!=actual.offset ||
           mh3_res_relative(actual.container,actual.offset,actual.offset+12,1,&parent)!=MH3_RES_OK || parent) {fail(&p,MH3_PREPARE_INVALID);break;}
        if(dictionary(&actual,MH3_RES_NODES,&plan->nodes) || dictionary(&actual,MH3_RES_SHAPES,&plan->shapes) || dictionary(&actual,MH3_RES_MATERIALS,&plan->materials)) {fail(&p,MH3_PREPARE_INVALID);break;}
        if(s->component_shape_count && s->component_shapes[s->component_shape_count-1]>=plan->shapes) {fail(&p,MH3_PREPARE_INVALID);break;}
        fail(&p,model_info(&actual,plan));if(p.status)break;
        fail(&p,nodes(&p,s,plan,0));if(p.status)break;fail(&p,bytecodes(&p,s,plan,0));if(p.status)break;
        for(uint32_t j=0;j<plan->materials;j++) {material mat;status=material_open(&actual,j,&mat);if(status) {fail(&p,status);break;}plan->bindings+=mat.bindings;}
        if(p.status)break;
        plan->starts[1]=counts[1];plan->starts[3]=counts[3];plan->starts[4]=counts[4];plan->starts[5]=counts[5];plan->starts[6]=counts[6];
        const uint32_t add[8]={0,plan->nodes,0,plan->shapes,plan->materials,plan->bindings,plan->draws,0};
        for(unsigned j=0;j<8;j++)if(add[j]>UINT32_MAX-counts[j])fail(&p,MH3_PREPARE_RANGE);else counts[j]+=add[j];
        if(!p.status)fail(&p,corners(&p,s,plan,0));
        if(plan->components>UINT32_MAX-counts[7])fail(&p,MH3_PREPARE_RANGE);else counts[7]+=plan->components;
    }
    counts[2]=p.layouts;
    if(!p.status)fail(&p,mh3_prepare_mesh_begin(&p.mesh,work->metadata,work->metadata_bytes,counts,work->gpu));
    if(!p.status)fail(&p,mh3_prepare_identity_begin(&p.identity,work->identities,work->identity_bytes,count,counts[3],counts[4]));
    for(uint32_t i=0;i<p.layouts && !p.status;i++) {
        const mh3_pack_layout *layout=&work->layouts[i];uint32_t v[40]={layout->count,layout->stride,layout->flags,0};
        for(uint32_t j=0;j<layout->count;j++) {const mh3_pack_attribute *a=&layout->attributes[j];v[4+j*2]=a->semantic|(uint32_t)a->offset<<16;v[5+j*2]=a->format|(uint32_t)a->components<<8;}
        record(&p,&p.mesh,2,v,4+layout->count*2);
    }
    for(uint32_t i=0;i<count && !p.status;i++) {
        const mh3_prepare_model_source *s=&sources[i];mh3_prepare_model_plan *plan=&work->plans[i];p.diagnostic.model_id=s->asset_id;
        fail(&p,nodes(&p,s,plan,1));if(p.status)break;fail(&p,materials(&p,s,plan));if(p.status)break;
        fail(&p,bytecodes(&p,s,plan,1));if(p.status)break;fail(&p,corners(&p,s,plan,1));if(p.status)break;fail(&p,emit_model(&p,s,plan));
    }
    mh3_prepared_model_pack result={0};
    if(!p.status)fail(&p,mh3_prepare_mesh_finish(&p.mesh,&result.pack));
    if(!p.status)fail(&p,identity(&p,sources,count));
    if(!p.status)fail(&p,mh3_prepare_identity_finish(&p.identity,&result.pack,&result.catalog));
    if(p.status) {
        if(p.mesh.data)fail(&p,mh3_prepare_abort(&p.mesh));
        int gpu_rollback=work->gpu.truncate(work->gpu.context,0);
        int index_rollback=work->shape.indices.truncate(work->shape.indices.context,0);
        if(gpu_rollback || index_rollback)p.status=MH3_PREPARE_ROLLBACK;
        if(work->metadata_bytes>=4)memset(work->metadata,0,4);if(work->identity_bytes>=4)memset(work->identities,0,4);
        if(diagnostic)*diagnostic=p.diagnostic;return p.status;
    }
    result.mesh=p.mesh;result.identity=p.identity;*out=result;return MH3_PREPARE_OK;
}
