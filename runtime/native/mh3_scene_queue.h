#ifndef MH3_SCENE_QUEUE_H
#define MH3_SCENE_QUEUE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { MH3_SCENE_CATEGORIES=6, MH3_SCENE_NODE_BYTES=52 };
typedef struct {
    uint32_t identity,object,callback;
    float depth,position[3],near_limit,far_limit;
    uint16_t flags;
    uint8_t phase,initialized,type,category;
} mh3_scene_node;
/* Identity values describe application objects and CPU link storage. The C
 * implementation never dereferences them or owns their lifetime. */
typedef struct {
    void *user;
    uint32_t (*read_link)(void *,uint32_t);
    void (*write_link)(void *,uint32_t,uint32_t);
    float (*read_depth)(void *,uint32_t);
} mh3_scene_links;
typedef struct {
    void *user;
    uint32_t (*count)(void *,unsigned);
    uint32_t (*identity)(void *,unsigned,uint32_t);
    void (*read)(void *,uint32_t,mh3_scene_node *);
    void (*phase)(void *,uint32_t,uint8_t);
    void (*initialized)(void *,uint32_t);
    int (*query)(void *,unsigned);
    void (*callback)(void *,uint32_t,uint32_t);
    void (*prepare)(void *,uint32_t);
    void (*apply_view)(void *,uint32_t,const float[12]);
    int (*visibility)(void *,uint32_t,const float[12]);
    float (*world_y)(void *,uint32_t,uint8_t);
    void (*enqueue)(void *,uint32_t,uint8_t,float,float);
} mh3_scene_walk;
enum { MH3_SCENE_QUERY_DISABLED, MH3_SCENE_QUERY_PLAYER_FILTER,
       MH3_SCENE_QUERY_PLAYER_INDEX, MH3_SCENE_QUERY_SECOND_FILTER };
int mh3_scene_bucket(float depth,double origin,double span,uint16_t count,uint32_t *out);
int mh3_scene_reset(const mh3_scene_links *,uint32_t first,uint16_t count);
int mh3_scene_insert(const mh3_scene_links *,uint32_t head,uint32_t node,uint8_t category);
uint32_t mh3_scene_next(const mh3_scene_links *,uint32_t *cursor);
int mh3_scene_phase_selected(uint8_t phase,uint8_t pass);
int mh3_scene_visible(mh3_scene_node *,const float view[12],const float position[3],float model_view_z);
void mh3_scene_traverse(const mh3_scene_walk *,const float view[12],uint8_t filter,
                      int height_mode,float camera_y,int category_mode,float origin,float span);
#ifdef __cplusplus
}
#endif
#endif
