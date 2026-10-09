#ifndef MH3_OWNED_SCENE_H
#define MH3_OWNED_SCENE_H
#include "mh3_model_draw_dispatch.h"
#include "mh3_model_submission.h"
#include "mh3_model_view.h"
#include "mh3_scene_queue.h"
#ifdef __cplusplus
extern "C" {
#endif

/* First serial provider, before installing a renderer thread. All identities
 * below index owned records. Neither the hot provider nor its callbacks may
 * dereference guest RAM. Immutable MDL0 bytes are read during compile only. */
typedef struct mh3_owned_local {
    mh3_matrix34 rt;
    mh3_vec3 scale;
    uint32_t flags;
    uint16_t matrix;
} mh3_owned_local;

typedef struct mh3_owned_tree {
    uint16_t destination, source; /* node index/parent matrix for2; dst/src for6 */
    uint8_t operation;
} mh3_owned_tree;

typedef struct mh3_owned_draw {
    mh3_model_draw_record record;
} mh3_owned_draw;

typedef struct mh3_owned_shape {
    int32_t matrix;
    uint8_t hidden;
} mh3_owned_shape;

/* A publication contains only fields owned by its actual native producers.
 * It is an immutable sparse overlay, not a complete desired GPU state. The
 * caller certifies external material data before assembling this record. */
typedef struct mh3_owned_material {
    const mh3_native_material_state *publication;
    uint32_t top_known;
} mh3_owned_material;

typedef struct mh3_owned_model_storage {
    mh3_owned_local *locals;
    size_t local_capacity;
    mh3_owned_tree *tree;
    size_t tree_capacity;
    mh3_owned_draw *draws;
    size_t draw_capacity;
    mh3_owned_shape *shapes;
    size_t shape_capacity;
    uint32_t *material_keys;
    size_t material_capacity;
} mh3_owned_model_storage;

typedef struct mh3_owned_model {
    const mh3_owned_local *locals;
    const mh3_owned_tree *tree;
    const mh3_owned_draw *draws;
    const mh3_owned_shape *shapes;
    const uint32_t *material_keys;
    const mh3_owned_material *materials;
    const mh3_prepared_mesh *meshes;
    uint32_t local_count, tree_count, matrix_count, view_count;
    uint32_t draw_first[2], draw_count[2], material_count, shape_count;
    uint64_t generation; /* immutable model+material+resident owner version */
} mh3_owned_model;

/* Cold compile checks the complete immutable owner once. Initial supported
 * subset: MDL0v11, Maya scaling, static ROT_ZERO nodes, no billboards/SSC,
 * no NodeMix, no indexed palette meshes, untextured/unlit publications,
 * no runtime callbacks/replacements. Authored count writes and both controls
 * for any active channel must establish this subset independently of prior
 * sticky bindings.
 * Admitted meshes/material overlays have the same IDs as their dictionaries.
 * Failure leaves out unchanged; scratch contents may change. No allocations. */
mh3_res_status mh3_owned_model_compile(const mh3_res_model *model, const mh3_owned_model_storage *storage, const mh3_owned_material *materials, uint32_t material_count,
                                       const mh3_prepared_mesh *meshes, uint32_t shape_count, uint64_t generation, mh3_owned_model *out);

typedef struct mh3_owned_object_input {
    const mh3_owned_model *model;
    mh3_matrix34 local; /* first subset: root object, no parent */
    uint64_t source_generation;
    uint32_t root_flags, options; /* bits0/1: opaque/translucent sorting only */
    uint32_t unsupported;         /* adapter declares animations/callbacks/parent/etc */
} mh3_owned_object_input;

typedef struct mh3_owned_member {
    mh3_scene_node node;
    uint32_t object_index;
} mh3_owned_member;

typedef struct mh3_owned_scene_input {
    mh3_matrix34 camera;
    float projection[16];
    const mh3_owned_member *members[2];
    uint32_t member_count[2];
    const mh3_owned_object_input *objects;
    uint32_t object_count;
    int query[4], height_mode, category_mode;
    float camera_height, origin, span, missing_depth;
    uint16_t bucket_count[6];
    uint8_t filter;
    uint64_t generation;
} mh3_owned_scene_input;

typedef struct mh3_owned_asset_pins {
    void *user;
    mh3_res_status (*retain)(void *, const mh3_owned_model *);
    void (*release)(void *, const mh3_owned_model *);
} mh3_owned_asset_pins;

typedef struct mh3_owned_scene_packet {
    mh3_owned_scene_input input;
    mh3_owned_member *members;
    size_t member_capacity;
    mh3_owned_object_input *objects;
    size_t object_capacity;
    mh3_owned_asset_pins pins;
    uint32_t pinned_objects, matrix_count, bucket_total;
    uint8_t admitted;
} mh3_owned_scene_packet;

/* Capture only copies membership and root/local inputs. No traversal, pose,
 * visibility, sorting, material merge or draw recipe is performed here.
 * The producer owns this slot exclusively. Capacity/class checks precede pin
 * changes; a retain failure rolls all new pins back. Replace/release requires
 * private slot ownership. Both empty source sets are legal. */
mh3_res_status mh3_owned_scene_capture(mh3_owned_scene_packet *, const mh3_owned_scene_input *, const mh3_owned_asset_pins *);
void mh3_owned_scene_release(mh3_owned_scene_packet *);

typedef struct mh3_owned_object_work {
    mh3_matrix34 world, view;
    uint32_t matrix_first;
    uint8_t prepared, viewed;
} mh3_owned_object_work;

typedef struct mh3_owned_scene_work {
    uint32_t *links, *buckets;
    size_t member_capacity, bucket_capacity;
    mh3_owned_object_work *objects;
    size_t object_capacity;
    mh3_matrix34 *world, *position;
    mh3_matrix33 *normal;
    mh3_vec3 *scale;
    uint32_t *attributes;
    size_t matrix_capacity;
    mh3_model_draw_record *sort;
    size_t sort_capacity;
} mh3_owned_scene_work;

/* Renderer context persists across CPU slot swaps, like the original sticky
 * bindings. Each submit borrows material/palette only for the call. The GPU
 * backend must copy uniforms or retain separate GPU storage before returning. */
typedef struct mh3_owned_render_state {
    mh3_native_material_state material;
    mh3_model_palette palette;
    int32_t sort_phase[2];
} mh3_owned_render_state;

typedef struct mh3_owned_scene_submission {
    mh3_resolved_mesh_submit mesh;
    const mh3_owned_object_input *object;
    uint32_t member_index, dispatch_flags; /* flags&16: same selected material */
    uint16_t node, material, shape;
    uint8_t translucent;
} mh3_owned_scene_submission;

typedef mh3_res_status (*mh3_owned_scene_emit)(void *, const mh3_owned_scene_submission *);
typedef struct mh3_owned_scene_stats {
    uint32_t members, visible, objects_prepared, world_matrices, view_matrices;
    uint32_t draws, material_publications, repeated_materials;
} mh3_owned_scene_stats;

/* Exclusive render/front ownership permits mutation of packet-local node
 * depth/phase/initialized and work arrays. Internal contracts are assertions;
 * NDEBUG removes them. This serial gate does not submit GXM or start threads. */
mh3_res_status mh3_owned_scene_render(mh3_owned_scene_packet *, mh3_owned_scene_work *, mh3_owned_render_state *, mh3_owned_scene_emit, void *, mh3_owned_scene_stats *);
#ifdef __cplusplus
}
#endif
#endif
