#include "mh3_scene_queue.h"

int mh3_scene_bucket(float depth, double origin, double span, uint16_t count, uint32_t *out) {
    float normalized, warped;

    if (!out || !count)
        return 0;

    normalized = (float)((double)(float)((double)depth - (double)origin) / (double)span);
    warped = normalized >= 0.1f ? 0.8f + 0.2222222089767456f * (1.0f - normalized) : (0.8f * normalized) / 0.1f;
    /* Ordered comparisons also preserve the original NaN -> first bin rule. */
    if (!(warped >= 0.0f))
        warped = 0.0f;
    else if (warped > 1.0f)
        warped = 1.0f;

    *out = (uint32_t)count - 1u - (uint32_t)((float)(count - 1u) * warped);
    return 1;
}

int mh3_scene_reset(const mh3_scene_links *io, uint32_t first, uint16_t count) {
    uint32_t i;

    if (!io || !io->write_link || !count || (first & 3u))
        return 0;

    for (i = 0; i < count; ++i)
        io->write_link(io->user, first + i * 4u, i + 1u < count ? first + (i + 1u) * 4u : 0);

    return 1;
}

int mh3_scene_insert(const mh3_scene_links *io, uint32_t head, uint32_t node, uint8_t category) {
    uint32_t link;

    if (!io || !io->read_link || !io->write_link || !io->read_depth || !node || (node & 3u))
        return 0;

    for (;;) {
        link = io->read_link(io->user, head);

        if (!link || !(link & 1u) || category == 0 || io->read_depth(io->user, link & ~1u) <= io->read_depth(io->user, node))
            break;

        head = link & ~1u;
    }

    io->write_link(io->user, head, node | 1u);
    io->write_link(io->user, node, link);
    return 1;
}

uint32_t mh3_scene_next(const mh3_scene_links *io, uint32_t *cursor) {
    uint32_t link;

    if (!io || !cursor || !io->read_link)
        return 0;
    while (*cursor) {
        link = io->read_link(io->user, *cursor);
        *cursor = link & ~1u;

        if (link & 1u)
            return *cursor;
    }

    return 0;
}

int mh3_scene_phase_selected(uint8_t phase, uint8_t pass) {
    return pass == 2 ? phase == 2 : pass == 3 ? phase != 2 : 1;
}

int mh3_scene_visible(mh3_scene_node *node, const float view[12], const float position[3], float model_view_z) {
    float dot;

    if (!node || !view)
        return 0;

    if (!(node->flags & 1u) && node->type < 2)
        node->depth = -model_view_z;
    else {
        if (!(node->flags & 1u) && node->type != 2)
            return 0;

        if (!position)
            return 0;

        dot = position[0] * view[8] + position[1] * view[9];
        dot += position[2] * view[10];
        node->depth = -(view[11] + dot);
    }

    return (!(node->flags & 0x80u) || -node->near_limit <= node->depth) && (!(node->flags & 0x100u) || node->depth <= node->far_limit);
}

void mh3_scene_traverse(const mh3_scene_walk *io, const float view[12], uint8_t filter, int height_mode, float camera_y, int category_mode, float origin, float span) {
    unsigned set;
    uint32_t i, object;
    mh3_scene_node node;

    for (set = 0; set < 2; ++set) {
        const uint32_t first = io->identity(io->user, set, 0);

        for (i = 0; i < io->count(io->user, set); ++i) {
            const uint32_t identity = first + i * MH3_SCENE_NODE_BYTES;
            io->read(io->user, identity, &node);

            if (!(node.flags & 0x200u) && io->query(io->user, MH3_SCENE_QUERY_DISABLED))
                continue;

            if ((node.flags & 0xc08u) && io->query(io->user, MH3_SCENE_QUERY_PLAYER_FILTER) == 1 &&
                (((node.flags & 0x400u) && io->query(io->user, MH3_SCENE_QUERY_PLAYER_INDEX) == 0) || ((node.flags & 0x800u) && io->query(io->user, MH3_SCENE_QUERY_PLAYER_INDEX) == 1) ||
                 (node.flags & 8u)))
                continue;
            if ((node.flags & 0x20u) && io->query(io->user, MH3_SCENE_QUERY_SECOND_FILTER) == 1)
                continue;

            io->phase(io->user, identity, 0);

            if (filter == 1 && !(node.flags & 2u))
                continue;

            if (node.type < 2) {
                object = node.object;

                if (!node.initialized) {
                    if ((node.flags & 0x40u) && node.callback)
                        io->callback(io->user, node.callback, identity);

                    io->prepare(io->user, object);
                    io->initialized(io->user, identity);
                }

                io->apply_view(io->user, object, view);
            }

            if (!io->visibility(io->user, identity, view))
                continue;

            io->read(io->user, identity, &node);

            if (node.category == 1 || node.category == 2) {
                float height = (node.flags & 1u) ? node.position[1] : io->world_y(io->user, node.type < 2 ? object : node.object, node.type);

                if (height_mode == 1)
                    io->phase(io->user, identity, camera_y <= 0.0f ? (height >= 0.0f ? 2 : 1) : (height >= 0.0f ? 1 : 2));
            } else if (node.category == 0 && category_mode == 1) {
                io->phase(io->user, identity, (node.flags & 4u) ? 2 : 1);
            }

            io->enqueue(io->user, identity, node.category, origin, span);
        }
    }
}
