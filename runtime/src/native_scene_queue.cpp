// Application scene buckets; SDK vertex-cache and completion commands are
// replaced by explicit native buffer ownership and ordered GPU submission.
#include "abi_bridge.h"
#include "fatal.h"
#include "game_step.h"
#include "guest_call.h"
#include "memory.h"
#include "frame_split.h"
#include "g3d_native.h"
#include "native_graphics.h"
#include "scene_cull.h"
#include "../native/mh3_scene_queue.h"
#include "../native/mh3_model_state.h"
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>
#include <array>

namespace {
constexpr uint32_t Root = 0x80796e34, Heads = 0x805984e0, Counts = 0x805984f8;
void ReadNode(void*, uint32_t address, mh3_scene_node* node) {
    *node = {};
    node->identity = address;
    node->depth = Memory::ReadFloat32(address + 4);
    node->phase = Memory::Read8(address + 8);
    node->initialized = Memory::Read8(address + 9);

    for (unsigned i = 0; i < 3; ++i)
        node->position[i] = Memory::ReadFloat32(address + 12 + i * 4);

    node->object = Memory::Read32(address + 24);
    node->type = Memory::Read8(address + 28);
    node->category = Memory::Read8(address + 29);
    node->flags = Memory::Read16(address + 30);
    node->callback = Memory::Read32(address + 32);
    node->near_limit = Memory::ReadFloat32(address + 36);
    node->far_limit = Memory::ReadFloat32(address + 40);
}

uint32_t ReadLink(void*, uint32_t address) {
    return Memory::Read32(address);
}

void WriteLink(void*, uint32_t address, uint32_t link) {
    Memory::Write32(address, link);
}

float ReadDepth(void*, uint32_t address) {
    return Memory::ReadFloat32(address + 4);
}

const mh3_scene_links links{nullptr, ReadLink, WriteLink, ReadDepth};
uint32_t Head(unsigned category) {
    if (category >= MH3_SCENE_CATEGORIES)
        MH3_THROW(std::runtime_error("Native scene category is out of range"));

    return Memory::Read32(Heads + category * 4);
}

void Reset(unsigned category) {
    if (!mh3_scene_reset(&links, Head(category), Memory::Read16(Counts + category * 2)))
        MH3_THROW(std::runtime_error("Native scene bucket storage is invalid"));
}

void ResetAll() {
    for (unsigned i = 0; i < MH3_SCENE_CATEGORIES; ++i)
        Reset(i);
}

void Enqueue(void*, uint32_t node, uint8_t category, double origin, double span) {
    uint32_t bin = 0;

    if (category >= MH3_SCENE_CATEGORIES || !mh3_scene_bucket(Memory::ReadFloat32(node + 4), origin, span, Memory::Read16(Counts + category * 2), &bin) ||
        !mh3_scene_insert(&links, Head(category) + bin * 4, node, category))
        MH3_THROW(std::runtime_error("Native scene bucket input is invalid"));
}

struct WalkAdapter {
    GuestCall call;
    uint32_t view_address, context;
};

uint32_t Count(void*, unsigned set) {
    return Memory::Read32(0x80796e68 + set * 4);
}

uint32_t Identity(void*, unsigned set, uint32_t index) {
    return Memory::Read32(0x80796e60 + set * 4) + index * 52;
}

void Phase(void*, uint32_t node, uint8_t phase) {
    Memory::Write8(node + 8, phase);
}

void Initialized(void*, uint32_t node) {
    Memory::Write8(node + 9, 1);
}

// The traverse's filters, read where their getters read them. The player's
// split-screen record (0x802BED34): the second one when the split byte
// (0x800470BC) and MyPlayerIndex are both set.
uint32_t PlayerRecord() {
    return Memory::Read8(0x8065b4d6) && Memory::Read8(0x8065aa87) ? 0x806be158 : 0x806bdc60;
}

int SecondFilter() {
    return Memory::Read8(PlayerRecord() + 0x284) == 1;
}

int Query(void*, unsigned kind) {
    switch (kind) {
    case MH3_SCENE_QUERY_DISABLED:
        return int8_t(Memory::Read8(0x8065b2d7));
    case MH3_SCENE_QUERY_PLAYER_FILTER:
        return !SecondFilter() && Memory::Read8(PlayerRecord() + 0x188) == 1;
    case MH3_SCENE_QUERY_PLAYER_INDEX:
        return int8_t(Memory::Read8(0x8065aa87));
    case MH3_SCENE_QUERY_SECOND_FILTER:
        return SecondFilter();
    default:
        MH3_THROW(std::runtime_error("Native scene query is invalid"));
    }
}

void Callback(void* user, uint32_t callback, uint32_t node) {
    auto& adapter = *static_cast<WalkAdapter*>(user);

    // A callback may use the effects the worker is calculating.
    EffectStep::Join(adapter.call.cpu);
    adapter.call(callback, {node});
}

void Prepare(void* user, uint32_t object) {
    static_cast<WalkAdapter*>(user)->call(0x8007F7E0, {object});
}

void ApplyView(void* user, uint32_t object, const float*) {
    auto& adapter = *static_cast<WalkAdapter*>(user);
    adapter.call.Virtual(object, 12, {4, 0, adapter.view_address});
}

// A scene object's root matrices (0x8007B5A8): world, then two more 3x4
// matrices, the view third. An effect's matrix (0x800A6124) is at +58.
constexpr uint32_t kObjectWorld = 0xC, kObjectView = 0x6C, kEffectMatrix = 0x58;

int Visibility(void*, uint32_t identity, const float view[12]) {
    mh3_scene_node node;
    ReadNode(nullptr, identity, &node);
    float point[3]{}, view_z = 0;

    if (node.flags & 1)
        for (unsigned i = 0; i < 3; ++i)
            point[i] = node.position[i];
    else if (node.type < 2)
        view_z = Memory::ReadFloat32(node.object + kObjectView + 44);
    else if (node.type == 2)
        for (unsigned i = 0; i < 3; ++i)
            point[i] = Memory::ReadFloat32(node.object + kEffectMatrix + 12 + i * 16);

    const int visible = mh3_scene_visible(&node, view, point, view_z);

    if ((node.flags & 1) || node.type <= 2)
        Memory::WriteFloat32(identity + 4, node.depth);

    return visible;
}

// A model's world matrix word +44, which the original reads from a copy
// (0x80081778); an effect's matrix word +28.
float WorldY(void*, uint32_t object, uint8_t type) {
    if (type < 2)
        return Memory::ReadFloat32(object + kObjectWorld + 44);

    if (type == 2)
        return Memory::ReadFloat32(object + kEffectMatrix + 28);

    return 0;
}

void EnqueueWalk(void* user, uint32_t node, uint8_t category, float, float) {
    const auto context = static_cast<WalkAdapter*>(user)->context;
    Enqueue(nullptr, node, category, Memory::ReadFloat32(context + 0xb4), Memory::ReadFloat32(context + 0xb8));
}

void Traverse(CpuContext* cpu, uint32_t camera, uint8_t filter) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_TRAVERSE);
    GuestFrame frame(cpu, 0xc0);
    WalkAdapter adapter{{cpu}, frame[0x40], 0};
    adapter.call(0x800753F8, {camera, adapter.view_address});
    const uint32_t context = adapter.call(0x80047298, {camera});
    adapter.context = context;
    const int height_mode = int32_t(adapter.call(0x802AFF9C));
    float camera_y = 0;

    if (height_mode == 1) {
        adapter.call(0x80074B0C, {camera, 0, frame[0x80], 0});
        camera_y = Memory::ReadFloat32(frame[0x80]);
    }

    const int category_mode = int32_t(adapter.call(0x802AFFBC));
    float view[12];

    for (unsigned i = 0; i < 12; ++i)
        view[i] = Memory::ReadFloat32(adapter.view_address + i * 4);

    const mh3_scene_walk walk{&adapter, Count, Identity, ReadNode, Phase, Initialized, Query, Callback, Prepare, ApplyView, Visibility, WorldY, EnqueueWalk};
    mh3_scene_traverse(&walk, view, filter, height_mode, camera_y, category_mode, Memory::ReadFloat32(context + 0xb4), Memory::ReadFloat32(context + 0xb8));
}

void DrawBucket(GuestCall& call, unsigned category, uint8_t pass) {
    uint32_t cursor = Head(category), identity;

    while ((identity = mh3_scene_next(&links, &cursor))) {
        mh3_scene_node node;
        ReadNode(nullptr, identity, &node);

        if (!mh3_scene_phase_selected(node.phase, pass))
            continue;

        if (!(node.flags & 0x40) && node.callback) {
            // A callback may use the effects the worker is calculating.
            EffectStep::Join(call.cpu);
            MH3_SPLIT_SCOPE(MH3_SPLIT_NODE_CALLBACK);
            call(node.callback, {identity});
        }

        // Callback changes are visible to the original subsequent draw loads.
        ReadNode(nullptr, identity, &node);

        if (node.type < 2) {
            MH3_SPLIT_SCOPE(MH3_SPLIT_MODEL);

            // Native lateral visibility (scene_cull.h); the original tests depth only.
            if (!SkipNativeSceneModel(node.object))
                call(0x8004978C, {node.object, pass == 1 ? 4u : 0xffffffffu});
        } else if (node.type == 2) {
            EffectStep::Join(call.cpu);
            MH3_SPLIT_SCOPE(MH3_SPLIT_EFFECT);
            const auto target = Memory::Read32(Memory::Read32(node.object + 28) + 28);
            call(target, {node.object, Memory::Read32(Memory::Read32(0x80796e30) + 0x29084)});
            call(0x800885F4, {0x7ff});
        }
    }
}

// The prepared buckets (SceneQueue::Prepare): each category's nodes in their
// order as the traverse queued them, with the fields read then. The buckets
// are lists threaded through the scene nodes in guest RAM, which the next
// logic step resets and reuses while the draw thread draws.
std::array<std::vector<std::pair<uint32_t, mh3_scene_node>>, MH3_SCENE_CATEGORIES> prepared;

// The guest objects the prepared buckets call into, sorted: each node's
// object and, for a model, its draw callback object (+0xD4, scene_callback in
// mh3_model_draw_dispatch.c). Written at the handoff, read by the logic step
// beside the draw.
std::vector<uint32_t> preparedObjects;

void Flatten() {
    preparedObjects.clear();

    for (unsigned category = 0; category < MH3_SCENE_CATEGORIES; ++category) {
        auto& nodes = prepared[category];
        nodes.clear();
        uint32_t cursor = Head(category), identity;

        while ((identity = mh3_scene_next(&links, &cursor))) {
            mh3_scene_node node;
            ReadNode(nullptr, identity, &node);
            nodes.emplace_back(identity, node);

            if (!node.object)
                continue;

            preparedObjects.push_back(node.object);

            if (node.type < 2)
                if (const uint32_t callback = Memory::Read32(node.object + 0xd4))
                    preparedObjects.push_back(callback);
        }
    }

    std::sort(preparedObjects.begin(), preparedObjects.end());
}

// DrawBucket over a prepared category: a node's callback cannot change the
// fields the draw then uses (they were read at the handoff).
void DrawPreparedBucket(GuestCall& call, unsigned category, uint8_t pass) {
    for (const auto& [identity, node] : prepared[category]) {
        if (!mh3_scene_phase_selected(node.phase, pass))
            continue;

        if (!(node.flags & 0x40) && node.callback) {
            EffectStep::Join(call.cpu);
            MH3_SPLIT_SCOPE(MH3_SPLIT_NODE_CALLBACK);
            call(node.callback, {identity});
        }

        if (node.type < 2) {
            MH3_SPLIT_SCOPE(MH3_SPLIT_MODEL);

            if (!SkipNativeSceneModel(node.object))
                call(0x8004978C, {node.object, pass == 1 ? 4u : 0xffffffffu});
        } else if (node.type == 2 && node.object) {
            EffectStep::Join(call.cpu);
            MH3_SPLIT_SCOPE(MH3_SPLIT_EFFECT);

            if (const uint32_t target = Memory::Read32(Memory::Read32(node.object + 28) + 28)) {
                call(target, {node.object, Memory::Read32(Memory::Read32(0x80796e30) + 0x29084)});
                call(0x800885F4, {0x7ff});
            }
        }
    }
}

void DrawAll(GuestCall& call, bool flattened = false) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_BUCKETS);
    constexpr uint8_t sequence[][2] = {{0, 2}, {5, 0}, {0, 3}, {2, 2}, {1, 2}, {4, 0}, {2, 3}, {1, 3}, {3, 0}};

    // Launched at the start of DrawScene, the effect calculation runs on the
    // guest worker through the traverse and the opaque buckets (categories 0
    // and 5: models); effect nodes and node callbacks join it first, and the
    // sorted buckets after them. A split scene launches it here.
    EffectStep::Launch(*call.cpu);

    for (const auto& entry : sequence) {
        if (entry[0] != 0 && entry[0] != 5)
            EffectStep::Join(call.cpu);

        if (flattened)
            DrawPreparedBucket(call, entry[0], entry[1]);
        else
            DrawBucket(call, entry[0], entry[1]);
    }

    EffectStep::Join(call.cpu);
}

void ResetIndirect() {
    auto material = GetNativeMaterialState();
    material.indirect_count = 0;
    constexpr uint64_t mask = ((uint64_t(1) << MH3_TEV_FIELD_COUNT) - 1) & ~((uint64_t(1) << MH3_TEV_INDIRECT_STAGE) - 1);

    for (auto& stage : material.tev) {
        stage.known |= mask;
        stage.indirect_stage = stage.indirect_format = stage.indirect_bias = stage.indirect_alpha = 0;
        stage.indirect_matrix = stage.indirect_wrap_s = stage.indirect_wrap_t = stage.indirect_original_lod = stage.indirect_add_previous = 0;
    }

    SetNativeMaterial(material);
}

// The single view's scene-graph part: the buckets' reset, the root's camera
// setup, the traverse and the root's view matrices (0x80082D40). With the
// draw thread it runs at the handoff on the game thread (SceneQueue::Prepare),
// so the draw thread walks only the buckets.
// A scene setup call over 20 ms (a one-off stall the split averages hide).
uint32_t TimedSetup(GuestCall& call, uint32_t function, std::initializer_list<uint32_t> args = {}) {
#ifdef MH3_DEVELOPMENT
    const auto start = MH3SplitClock::now();
    const uint32_t result = call(function, args);
    const auto elapsed = std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();

    if (elapsed >= 20)
        std::printf("MH3 slow scene setup call %08x: %.1f ms\n", function, elapsed);

    return result;
#else
    return call(function, args);
#endif
}

void PrepareSingle(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x30);
    GuestCall call{cpu};
    ResetAll();
    const uint32_t root = Memory::Read32(Root);

    if (!root)
        return;

    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE_SETUP);
        Memory::Write32(frame[0x10], TimedSetup(call, 0x80082C30, {root}));
    }

    Traverse(cpu, frame[0x10], 0);
    MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE_SETUP);
    TimedSetup(call, 0x80082D40, {root});
}

// The rest: the effect view, the buckets, the model caches' finish, the post effects.
void Finish(GuestCall& call, bool split);

void DrawPreparedSingle(CpuContext* cpu, bool flattened = false) {
    GuestFrame frame(cpu, 0x30);
    GuestCall call{cpu};
    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE_SETUP);
        TimedSetup(call, 0x800D4910);
    }

    DrawAll(call, flattened);
    Finish(call, false);
}

void DrawScene(CpuContext* cpu, bool split) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE);

    // The task pass's effect calculation overlaps the traverse and the opaque
    // buckets: neither reads what Effect::Calc writes (the traverse reads the
    // effects' root matrices, which their owners set), and the effect view
    // matrix update before the buckets waits for the join (MH3_EffectSystemView).
    if (!split)
        EffectStep::Launch(*cpu);

    GuestFrame frame(cpu, 0x30);
    GuestCall call{cpu};
    ResetAll();
    const uint32_t root = Memory::Read32(Root);

    if (!split) {
        PrepareSingle(cpu);
        DrawPreparedSingle(cpu);
        return;
    }

    if (root) {
        const uint32_t old = call(0x80049784);

        for (unsigned player = 0; player < 2; ++player) {
            if (player)
                ResetAll();

            call(0x800CF3F8, {player});
            call(0x80082C6C, {root, Memory::Read8(0x8065b50c + player)});
            call(0x802B2E90);
            Memory::Write32(frame[0x10], TimedSetup(call, 0x80082C30, {root}));
            Traverse(cpu, frame[0x10], 0);
            call(0x80082D40, {root});
            call(0x800D4910);
            DrawAll(call);

            // The two views submit in order. Native frame slots own their
            // uniforms/vertices until completion; no inter-view GPU wait.
        }

        call(0x800CF3F8, {0});
        call(0x80082C6C, {root, old});
        call(0x80082D40, {root});
    }

    Finish(call, true);
}

void Finish(GuestCall& call, bool split) {
    {
        MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE_FINISH);
        call(0x800885F4, {4});
        ResetIndirect();
    }

    MH3_SPLIT_SCOPE(MH3_SPLIT_POST);

    if (split) {
        call(0x800583AC);
        call(0x80058460);
    } else if (!int8_t(call(0x800D0788)))
        call(0x80059270);
}
} // namespace

namespace SceneQueue {
void Prepare(CpuContext* cpu) {
    EffectStep::Launch(*cpu);
    PrepareGraph(cpu);
}

// The walk's view passes run when the scene draws (g3d_calc_world.cpp).
void PrepareGraph(CpuContext* cpu) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE);
    G3dNative::DeferViewPasses(true);
    PrepareSingle(cpu);
    G3dNative::DeferViewPasses(false);
    Flatten();
}

void DrawPrepared(CpuContext* cpu) {
    MH3_SPLIT_SCOPE(MH3_SPLIT_SCENE);
    G3dNative::RunDeferredViewPasses();
    DrawPreparedSingle(cpu, true);
}

bool HoldsPrepared(uint32_t begin, uint32_t end) {
    const auto first = std::lower_bound(preparedObjects.begin(), preparedObjects.end(), begin);
    return first != preparedObjects.end() && *first < end;
}
} // namespace SceneQueue

extern "C" void MH3_SceneQueueReset(CpuContext*) {
    ResetAll();
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3E44, MH3_SceneQueueReset, "SceneQueueReset");
extern "C" void MH3_SceneQueueTraverse(CpuContext* cpu) {
    Traverse(cpu, cpu->gpr[3], uint8_t(cpu->gpr[4]));
}
REGISTER_NATIVE_FUNCTION_AS(0x800E3E80, MH3_SceneQueueTraverse, "SceneQueueTraverse");
extern "C" void MH3_SceneQueueVisible(CpuContext* cpu) {
    float view[12];

    for (unsigned i = 0; i < 12; ++i)
        view[i] = Memory::ReadFloat32(cpu->gpr[4] + i * 4);

    cpu->gpr[3] = Visibility(nullptr, cpu->gpr[3], view);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E41AC, MH3_SceneQueueVisible, "SceneQueueVisible");
extern "C" void MH3_SceneQueueEnqueue(CpuContext* cpu) {
    Enqueue(nullptr, cpu->gpr[3], uint8_t(cpu->gpr[4]), cpu->fpr[1].d, cpu->fpr[2].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E42E8, MH3_SceneQueueEnqueue, "SceneQueueEnqueue");
extern "C" void MH3_SceneQueueDraw(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x10);
    GuestCall call{cpu};
    DrawBucket(call, uint8_t(frame.Arg(3)), uint8_t(frame.Arg(4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x800E44B0, MH3_SceneQueueDraw, "SceneQueueDraw");
extern "C" void MH3_DrawScene(CpuContext* cpu) {
    DrawScene(cpu, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049498, MH3_DrawScene, "DrawScene");
extern "C" void MH3_DrawSplitScene(CpuContext* cpu) {
    DrawScene(cpu, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049594, MH3_DrawSplitScene, "DrawSplitScene");
extern "C" void MH3_SceneBlendMode(CpuContext* cpu) {
    auto raster = GetNativeDrawState().raster;
    raster.src_factor = uint8_t(cpu->gpr[3]) <= 7 ? uint8_t(cpu->gpr[3]) : 4;

    if (uint8_t(cpu->gpr[4]) > 7)
        MH3_THROW(std::runtime_error("Scene blend destination is outside its application domain"));

    raster.dst_factor = uint8_t(cpu->gpr[4]);
    raster.logic_op = 0;
    const auto mode = uint8_t(cpu->gpr[5]);
    raster.blend_mode = mode == 0 ? 0 : mode == 3 ? 3 : 1;
    SetNativeRaster(raster);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E45D8, MH3_SceneBlendMode, "SceneBlendMode");
extern "C" void MH3_SceneDepthMode(CpuContext* cpu) {
    constexpr uint8_t compare[] = {0, 1, 3, 2, 5, 6, 4, 7};
    auto raster = GetNativeDrawState().raster;
    const auto selector = uint8_t(cpu->gpr[4]);
    raster.depth_test = cpu->gpr[3] != 0;
    raster.depth_compare = raster.depth_test ? (selector < 8 ? compare[selector] : 3) : 3;
    raster.depth_write = uint8_t(cpu->gpr[5]) != 0;
    SetNativeRaster(raster);
}
REGISTER_NATIVE_FUNCTION_AS(0x800E46EC, MH3_SceneDepthMode, "SceneDepthMode");
