#include "mh3_particle_draw_state.h"
#include <math.h>
#include <string.h>

void mh3_particle_directional_rotation(uint8_t kind,const float sc[3][2],float out[12]) {
    const float sx=sc[0][0],cx=sc[0][1],sy=sc[1][0],cy=sc[1][1],sz=sc[2][0],cz=sc[2][1];
    mh3_draw_identity_position(out);
    if(kind==0) {out[5]=cx;out[6]=-sx;out[9]=sx;out[10]=cx;}
    else if(kind==1) {out[0]=cy;out[2]=sy;out[8]=-sy;out[10]=cy;}
    else if(kind==2) {out[0]=cz;out[1]=-sz;out[4]=sz;out[5]=cz;}
    else {
        out[0]=cy*cz;out[1]=(sx*sy)*cz-cx*sz;out[2]=(cx*sy)*cz+sx*sz;
        out[4]=cy*sz;out[5]=(sx*sy)*sz+cx*cz;out[6]=(cx*sy)*sz-sx*cz;
        out[8]=-sy;out[9]=cy*sx;out[10]=cx*cy;
    }
}
void mh3_particle_directional_size(const float pivot[2],float w,float h,float stretch,
                                   uint8_t kind,const float rotation[12],float out[12]) {
    float result[12],wp=w*pivot[0];
    if(!kind) {
        const float hs=h*stretch,shift=h*((pivot[1]+stretch)-1.0f);
        for(uint32_t r=0;r<3;r++) {
            const float *m=rotation+4*r;float *o=result+4*r;
            o[0]=m[0]*w;o[1]=m[1]*hs;o[2]=m[2]*w;
            o[3]=(-m[0]*wp)-m[1]*shift;
            if(r<2)o[3]=pivot[r]+o[3];
        }
    } else {
        const float hp=h*pivot[1];
        for(uint32_t r=0;r<3;r++) {
            const float *m=rotation+4*r;float *o=result+4*r;
            o[0]=m[0]*w;o[1]=m[2]*h;o[2]=m[1]*w;
            o[3]=(-m[0]*wp)-m[2]*hp;
            if(r==0)o[3]=pivot[0]+o[3];
            else if(r==2)o[3]=o[3]-pivot[1];
        }
    }
    memcpy(out,result,sizeof(result));
}
mh3_res_status mh3_particle_directional_draw(const mh3_particle_directional_scene *scene,
                                            const mh3_particle_directional_record *p,
                                            const float history[3],mh3_particle_directional_output *out) {
    if(!scene||!p||!history||!out)return MH3_RES_RANGE;
    if(scene->screen_facing>1||scene->plane_count<1||scene->plane_count>2)return MH3_RES_INVALID;
    mh3_particle_directional_output result;memset(&result,0,sizeof(result));
    mh3_effect_directional_inputs inputs;memset(&inputs,0,sizeof(inputs));
    memcpy(inputs.view_local,scene->view_local,sizeof(inputs.view_local));
    memcpy(inputs.fallback,scene->fallback,sizeof(inputs.fallback));
    memcpy(inputs.position,p->position,sizeof(inputs.position));
    memcpy(inputs.direction,p->direction,sizeof(inputs.direction));
    memcpy(inputs.history,history,sizeof(inputs.history));inputs.screen_facing=scene->screen_facing;
    float stretch=1.0f;
    if(scene->stretch) {
        const float magnitude=sqrtf((p->movement[0]*p->movement[0]+p->movement[1]*p->movement[1])+p->movement[2]*p->movement[2]);
        stretch=1.0f+(scene->movement_scale*magnitude)/p->height;
    }
    float rotation[12];mh3_particle_directional_rotation(scene->rotation_kind,p->sincos,rotation);
    mh3_particle_directional_size(scene->pivot,p->width,p->height,stretch,scene->size_kind,rotation,inputs.size_matrix);
    mh3_effect_directional_matrix(&inputs,result.history,result.matrix);
    result.plane_count=scene->plane_count;
    for(uint32_t plane=0;plane<result.plane_count;plane++) {
        if(scene->screen_facing) {
            float center[3],a[3],b[3];
            for(uint32_t r=0;r<3;r++) {
                center[r]=result.matrix[4*r+3];a[r]=result.matrix[4*r+plane]+result.matrix[4*r+plane+1];
                b[r]=result.matrix[4*r+plane]-result.matrix[4*r+plane+1];
            }
            mh3_effect_quad_axes(center,a,b,&result.quads[plane]);
        } else mh3_effect_quad_transform(result.matrix,&scene->canonical[plane],&result.quads[plane]);
    }
    *out=result;return MH3_RES_OK;
}
