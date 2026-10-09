// Model instances drawn on the render thread (model_instance_bridge.h).
//
// The game thread captures an eligible scene draw instead of performing it:
// the owner, its draw options, its view arrays (pose) and the guest memory
// lines its draw is known to read. The render thread draws it in packet order
// under a guest memory view (draw_memory.h) made of those captured bytes, the
// G3D state the draw writes as the game had it at the capture, and a scratch
// region for the words the draw passes by reference. Reads outside the
// captured lines are served from live memory and reported, so that owner's
// next capture includes them. Resource state comes from the render replica
// (draw_replica.h); the drawing state the game selected before the scene
// (matrix selectors, lights) comes along with the instance, and the frame
// packet record restores the rest.
#include "memory.h"
#include "draw_memory.h"
#include "draw_replica.h"
#include "native_frame_packet.h"
#include "native_shade_snapshot.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_instance_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_draw_dispatch.h"
#include "runtime/native/mh3_model_runtime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

const mh3_res_memory& MH3NativeResourceMemory();
bool NativeRenderThreadRunning();
namespace {
constexpr uint32_t ScratchBase = 0x8f000000, ScratchBytes = 0x100, LineBytes = 64;
// Learned lines are kept: forgetting them would send the next draws back to
// live memory, which the game thread is writing. Lines a moved heap left
// behind only cost their copy; an owner that gathers too many starts over.
constexpr size_t LearnedLines = 1024;
// Guest state the draw writes. A packet copies it at its first instance and
// each later instance carries the lines that changed since the one before, so
// every instance starts from the game's state at its capture. The render
// thread still forgets the cache fields whose host state it reselects.
struct Block {
    uint32_t address, bytes;
    bool copy;
};
constexpr Block Blocks[] = {
    {0x8066d300, MH3_MODEL_SORT_CAPACITY * 12, false}, // sorted draw records
    {0x80685300, 0x8068a7c8 - 0x80685300, true},       // G3D caches, texture/palette/fog/light state, scene views
    {0x80793680, 0xc0, true},                          // sort phase, indirect operator pointer
    {0x80796d80, 0x80, true},                          // cache flags
};
constexpr uint32_t OperatorPointer = 0x80793708, OperatorBytes = 152;
struct Run {
    uint32_t address, bytes;
    size_t offset;
};
#ifdef MH3_DEVELOPMENT
// The drawing state of one prepared item (validation).
struct ItemState {
    NativeDrawSnapshot draw;
    NativeMaterialSnapshot material;
    NativeLightingSnapshot lighting;
    NativePostSnapshot post;
    NativeTextureSnapshot textures;
    const void* prepared;
};
#endif
struct Instance {
    uint32_t owner, options, sources[3];
    size_t item_first, item_count;
    // Draw state the game selected before the scene (the guest caches skip
    // reselecting it inside): matrix selectors, lights and fog.
    mh3_model_matrix_selectors selectors;
    NativeLightingSnapshot lighting;
    std::array<mh3_native_light_set_entry, 4> lightSets;
    size_t lightSetCount;
    mh3_matrix_sources pose;
    size_t pose_offset[3];
    uint32_t pose_address[3];
    size_t run_first, run_count;
    size_t delta_first, delta_count;
};
// One packet's instances, indexed by its exchange slot like the model stores.
struct Slot {
    uint64_t sequence = UINT64_MAX;
    std::vector<Instance> instances;
    std::vector<Run> runs;
    std::vector<uint8_t> bytes; // captured lines and poses
    std::vector<Block> blocks;
    std::vector<size_t> block_offset;
    std::vector<Run> deltas;     // address: offset into base/state
    std::vector<uint8_t> base;   // the copied blocks at the packet's first capture
    // Render thread: base with the deltas so far, and the working copy the
    // current instance writes.
    std::vector<uint8_t> mirror, state;
    uint64_t mirrored = 0;
    std::array<uint8_t, ScratchBytes> scratch{};
#ifdef MH3_DEVELOPMENT
    std::vector<ItemState> items;
#endif
};
std::array<Slot, 3> slots;
bool enabled = false, validate = false;
#ifdef MH3_DEVELOPMENT
// Game thread: the instance whose draw notes items; render thread: the
// instance being compared and its next item.
Slot* noting = nullptr;
thread_local const Instance* checking = nullptr;
thread_local Slot* checkingSlot = nullptr;
thread_local size_t checked = 0;
thread_local bool mismatched = false;
thread_local unsigned reported = 0; // this instance
thread_local std::vector<const void*> preparedOrder;
unsigned mismatches = 0;
void Note(ItemState& state, const void* prepared) {
    std::memset(static_cast<void*>(&state), 0, sizeof state); // padding compares too
    CaptureNativeDrawSnapshot(state.draw);
    CaptureNativeMaterialSnapshot(state.material);
    CaptureNativeLightingSnapshot(state.lighting);
    CaptureNativePostSnapshot(state.post);
    CaptureNativeTextureSnapshot(state.textures);
    // Material revisions are per thread. Items set the position matrix when
    // they execute, which the game does later: its value here is a leftover.
    state.material.material.generation = 0;
    std::memset(state.draw.draw.position, 0, sizeof state.draw.draw.position);
    state.prepared = prepared;
}
template <class T>
bool Same(const T& a, const T& b) {
    return !std::memcmp(&a, &b, sizeof a);
}
// Animated matrices may read a live word a little later on the render thread.
bool Close(const NativePostSnapshot& a, const NativePostSnapshot& b) {
    if (a.known != b.known || a.matrix3x4 != b.matrix3x4)
        return false;
    for (unsigned i = 0; i < 8; i++)
        for (unsigned j = 0; j < 12; j++)
            if (std::fabs(a.post[i][j] - b.post[i][j]) > 1e-2f * (1.f + std::fabs(a.post[i][j])))
                return false;
    return true;
}
// Prints the first differing bytes of two snapshots by field.
template <class T>
void Dump(const T& expected, const T& actual, const std::vector<std::pair<const char*, size_t>>& fields) {
    const auto* x = reinterpret_cast<const uint8_t*>(&expected);
    const auto* y = reinterpret_cast<const uint8_t*>(&actual);
    unsigned shown = 0;
    for (size_t i = 0; i < sizeof(T) && shown < 16; i++)
        if (x[i] != y[i]) {
            const char* field = "?";
            size_t base = 0;
            for (const auto& f : fields)
                if (f.second <= i) {
                    field = f.first;
                    base = f.second;
                }
            std::fprintf(stderr, "  %s+%zu: %02x vs %02x\n", field, i - base, x[i], y[i]);
            ++shown;
        }
}
#endif
// Game thread: the copied blocks as the last capture saw them.
std::vector<uint8_t> mirror;
// Lines each owner's draw read beyond its captured bytes (game thread).
std::unordered_map<uint32_t, std::vector<uint32_t>> learned;
std::mutex missLock;
std::vector<std::pair<uint32_t, uint32_t>> missed;
// Render thread: the owner being drawn and the lines it missed.
thread_local uint32_t drawing = 0;
thread_local std::vector<uint32_t> misses;
thread_local std::vector<mh3_res_region> regions;
unsigned failures = 0;

uint32_t Canonical(uint32_t address) {
    return (address & 0x3fffffffu) | 0x80000000u;
}
void Miss(uint32_t address, size_t bytes) {
    if (!drawing || !bytes)
        return;
    const uint64_t start = Canonical(address) & ~uint64_t(LineBytes - 1), end = uint64_t(Canonical(address)) + bytes;
    for (uint64_t line = start; line < end; line += LineBytes)
        misses.push_back(uint32_t(line));
}
// Game thread: the recording view of a learning draw. Its one region lies
// outside guest RAM, so every MEM1 read reaches the miss hook.
thread_local mh3_res_memory recorder;
thread_local mh3_res_region recorderRegion;
thread_local uint8_t recorderBytes[4];
void Learn() {
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    {
        std::lock_guard lock(missLock);
        taken.swap(missed);
    }
    for (const auto& [owner, line] : taken) {
        auto& lines = learned[owner];
        if (lines.size() >= LearnedLines) {
            learned.erase(owner); // the next draw learns it again
            continue;
        }
        const auto at = std::lower_bound(lines.begin(), lines.end(), line);
        if (at == lines.end() || *at != line)
            lines.insert(at, line);
    }
}
// Removes [start, end) from the ascending disjoint runs.
void Subtract(std::vector<std::pair<uint32_t, uint32_t>>& runs, uint32_t start, uint32_t end) {
    std::vector<std::pair<uint32_t, uint32_t>> kept;
    kept.reserve(runs.size() + 1);
    for (const auto& [a, b] : runs) {
        if (b <= start || a >= end) {
            kept.emplace_back(a, b);
            continue;
        }
        if (a < start)
            kept.emplace_back(a, start);
        if (b > end)
            kept.emplace_back(end, b);
    }
    runs.swap(kept);
}
size_t Append(Slot& slot, const uint8_t* data, size_t bytes);
// Records the copied-block lines that changed since the previous capture.
void Delta(Slot& slot, Instance& instance) {
    instance.delta_first = slot.deltas.size();
    for (size_t i = 0; i < slot.blocks.size(); i++) {
        const auto& block = slot.blocks[i];
        if (!block.copy)
            continue;
        const auto* live = Memory::GetPointer(block.address, block.bytes);
        auto* seen = mirror.data() + slot.block_offset[i];
        for (uint32_t at = 0; at < block.bytes; at += LineBytes) {
            const uint32_t bytes = std::min(LineBytes, block.bytes - at);
            if (!std::memcmp(seen + at, live + at, bytes))
                continue;
            std::memcpy(seen + at, live + at, bytes);
            const size_t offset = slot.block_offset[i] + at;
            auto& last = slot.deltas;
            if (last.size() > instance.delta_first && last.back().address + last.back().bytes == offset) {
                last.back().bytes += bytes;
                (void)Append(slot, live + at, bytes); // contiguous with the previous line
            } else
                last.push_back({uint32_t(offset), bytes, Append(slot, live + at, bytes)});
        }
    }
    instance.delta_count = slot.deltas.size() - instance.delta_first;
}
Slot& Begin() {
    auto& slot = slots.at(NativeFramePacketSlot());
    if (slot.sequence == NativeFramePacketSequence())
        return slot;
    slot.sequence = NativeFramePacketSequence();
    slot.instances.clear();
#ifdef MH3_DEVELOPMENT
    slot.items.clear();
#endif
    slot.runs.clear();
    slot.deltas.clear();
    slot.bytes.clear();
    slot.blocks.assign(std::begin(Blocks), std::end(Blocks));
    // Indirect materials write their operator object through its pointer.
    if (const uint32_t op = Memory::Read32(OperatorPointer)) {
        const uint32_t address = Canonical(op);
        bool overlaps = false;
        for (const auto& block : slot.blocks)
            overlaps |= address < block.address + block.bytes && block.address < address + OperatorBytes;
        if (!overlaps)
            slot.blocks.push_back({address, OperatorBytes, true});
    }
    std::sort(slot.blocks.begin(), slot.blocks.end(), [](const Block& a, const Block& b) { return a.address < b.address; });
    slot.block_offset.clear();
    size_t total = 0;
    for (const auto& block : slot.blocks) {
        slot.block_offset.push_back(total);
        total += block.bytes;
    }
    slot.base.resize(total);
    for (size_t i = 0; i < slot.blocks.size(); i++) {
        const auto& block = slot.blocks[i];
        if (block.copy)
            std::memcpy(slot.base.data() + slot.block_offset[i], Memory::GetPointer(block.address, block.bytes), block.bytes);
    }
    mirror = slot.base;
    Learn();
    return slot;
}
size_t Append(Slot& slot, const uint8_t* data, size_t bytes) {
    const size_t offset = slot.bytes.size();
    slot.bytes.insert(slot.bytes.end(), data, data + bytes);
    return offset;
}
void Report(uint32_t owner, const char* what) {
    if (failures >= 32)
        return;
    std::fprintf(stderr, "MH3 model instance %08x: %s%s\n", owner, what, ++failures == 32 ? " (further failures are not reported)" : "");
}
} // namespace

bool NativeModelInstancesEnabled() {
    return enabled && NativeRenderThreadRunning();
}
void StartNativeModelInstances() {
    const char* value = std::getenv("MH3_MODEL_INSTANCES");
    if (!value || (value[0] != '1' && value[0] != '2'))
        return;
#ifdef MH3_DEVELOPMENT
    validate = value[0] == '2';
#endif
    SeedNativeCatalogReplica();
    SeedNativeTextureObjectReplica();
    SeedNativeVertexReplica();
    DrawReplica::sink = QueueNativeReplicaTask;
    mh3_res_region_miss = Miss;
    enabled = true;
    std::fprintf(stderr, "MH3 model instances: %s on the render thread\n", validate ? "validated" : "drawn");
}
void StopNativeModelInstances() {
    DrawReplica::sink = nullptr;
    enabled = false;
}

bool CaptureNativeModelInstance(uint32_t owner, uint32_t options, bool* learn) {
    *learn = false;
    // Scenes drawn inside a learning draw stay on the game thread.
    if (!NativeModelInstancesEnabled() || !NativeFramePacketRecording() || DrawMemory::CurrentView())
        return false;
    // The render thread runs only the native scene task, without callbacks.
    if (Memory::Read32(Memory::Read32(owner) + 0x0c) != 0x8007de60 || Memory::Read32(owner + 0xd4))
        return false;
    if (!learned.contains(Canonical(owner))) {
        *learn = true;
        return false;
    }
    Instance instance{};
    instance.owner = owner;
    instance.options = options;
    try {
        for (unsigned kind = 0; kind < 3; kind++)
            instance.sources[kind] = NativeModelViewArray(owner, kind);
        if (!instance.sources[0])
            return false;
        instance.pose = ReadNativeModelPose(instance.sources);
        instance.selectors = ReadNativeModelSelectors();
        CaptureNativeLightingSnapshot(instance.lighting);
        instance.lightSetCount = ReadNativeLightSets(instance.lightSets.data(), instance.lightSets.size());
        // Admits the model's pack here; the render replica sees it mirrored.
        (void)ResolveNativeModelOwner(Memory::Read32(owner + 0xe8));
    } catch (const std::exception&) {
        return false; // the game thread draw reports it
    }
    auto& slot = Begin();
    Delta(slot, instance);
    // Pose tables become regions at their guest addresses.
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    const mh3_matrix_table* tables[3]{&instance.pose.positions, &instance.pose.normals, &instance.pose.environment};
    for (unsigned i = 0; i < 3; i++) {
        const auto& span = tables[i]->bytes;
        instance.pose_address[i] = 0;
        if (!span.data || !span.size)
            continue;
        instance.pose_offset[i] = Append(slot, span.data, span.size);
        if (instance.sources[i] && Memory::GetPointer(instance.sources[i], span.size) == span.data)
            instance.pose_address[i] = Canonical(instance.sources[i]);
    }
    // The owner's learned lines, merged into runs, minus blocks and poses.
    const auto found = learned.find(Canonical(owner));
    if (found != learned.end())
        for (const uint32_t line : found->second) {
            if (!runs.empty() && runs.back().second == line)
                runs.back().second = line + LineBytes;
            else
                runs.emplace_back(line, line + LineBytes);
        }
    for (const auto& block : slot.blocks)
        Subtract(runs, block.address, block.address + block.bytes);
    for (unsigned i = 0; i < 3; i++)
        if (instance.pose_address[i])
            Subtract(runs, instance.pose_address[i], instance.pose_address[i] + uint32_t(tables[i]->bytes.size));
    instance.run_first = slot.runs.size();
    for (const auto& [start, end] : runs) {
        const uint32_t bytes = end - start;
        slot.runs.push_back({start, bytes, Append(slot, Memory::GetPointer(start, bytes), bytes)});
    }
    instance.run_count = slot.runs.size() - instance.run_first;
#ifdef MH3_DEVELOPMENT
    instance.item_first = slot.items.size();
#endif
    slot.instances.push_back(instance);
    RecordNativeModelInstance(uint32_t(slot.instances.size() - 1));
#ifdef MH3_DEVELOPMENT
    if (validate) {
        noting = &slot; // the game thread draws it too and notes its items
        return false;
    }
#endif
    return true;
}
NativeModelInstanceLearning::NativeModelInstanceLearning(uint32_t owner_) : owner(Canonical(owner_)) {
    recorder = MH3NativeResourceMemory();
    recorderRegion = {ScratchBase, {recorderBytes, sizeof recorderBytes}, 0};
    recorder.regions = &recorderRegion;
    recorder.region_count = 1;
    DrawMemory::InstallView(&recorder);
    DrawMemory::liveWrites = true;
    drawing = owner;
    misses.clear();
}
NativeModelInstanceLearning::~NativeModelInstanceLearning() {
    DrawMemory::InstallView(nullptr);
    DrawMemory::liveWrites = false;
    drawing = 0;
    std::sort(misses.begin(), misses.end());
    misses.erase(std::unique(misses.begin(), misses.end()), misses.end());
    if (misses.size() <= LearnedLines)
        learned[owner] = misses; // known, even when empty
    misses.clear();
}
#ifdef MH3_DEVELOPMENT
void FinishNativeModelInstance() {
    if (!noting)
        return;
    auto& instance = noting->instances.back();
    instance.item_count = noting->items.size() - instance.item_first;
    noting = nullptr;
}
bool NoteNativeModelInstanceItem(const void* prepared) {
    if (DrawReplica::Current() == DrawReplica::Game) {
        if (noting)
            Note(noting->items.emplace_back(), prepared);
        return false;
    }
    if (!checking)
        return false;
    const size_t index = checked++;
    preparedOrder.push_back(prepared);
    const char* differs = nullptr;
    if (index >= checking->item_count)
        differs = "extra item";
    else {
        ItemState state;
        Note(state, prepared);
        const auto& expected = checkingSlot->items[checking->item_first + index];
        differs = expected.prepared != state.prepared        ? "mesh"
                  : !Same(expected.material, state.material) ? "material"
                  : !Same(expected.lighting, state.lighting) ? "lighting"
                  : !Same(expected.textures, state.textures) ? "textures"
                  : !Same(expected.draw, state.draw)         ? "draw"
                  : !Close(expected.post, state.post)        ? "post"
                                                             : nullptr;
    }
    if (differs && mismatches < 400 && reported < 6) {
        mismatched = true;
        ++reported;
        std::fprintf(stderr, "MH3 model instance %08x item %zu/%zu: %s differs (%zu lines read live)\n", checking->owner, index, checking->item_count, differs, misses.size());
        if (index < checking->item_count) {
            ItemState state;
            Note(state, prepared);
            const auto& expected = checkingSlot->items[checking->item_first + index];
            using L = NativeLightingSnapshot;
            using M = mh3_native_material_state;
            using T = NativeTextureSnapshot;
            using B = mh3_native_texture_binding;
            if (differs[0] == 'l')
                Dump(expected.lighting, state.lighting,
                     {{"lights", 0}, {"preparedLights", offsetof(L, preparedLights)}, {"fog", offsetof(L, fog)}, {"preparedFog", offsetof(L, preparedFog)}, {"fogKnown", offsetof(L, fogKnown)}});
            else if (differs[0] == 'm' && differs[1] == 'a')
                Dump(expected.material, state.material,
                     {{"counts", 0}, {"tev", offsetof(M, tev)}, {"color", offsetof(M, color)}, {"konst", offsetof(M, konst)}, {"swaps", offsetof(M, swaps)},
                      {"indirect", offsetof(M, indirect)}, {"indirect_matrix", offsetof(M, indirect_matrix)}, {"texgen", offsetof(M, texgen)},
                      {"channel", offsetof(M, channel)}, {"pixel", offsetof(M, pixel)}, {"clip", offsetof(M, clip_mode_known)},
                      {"depthBeforeTexture", offsetof(NativeMaterialSnapshot, depthBeforeTexture)}});
            else if (differs[0] == 'd') {
                using D = mh3_draw_state;
                Dump(expected.draw, state.draw,
                     {{"projection", 0}, {"position", offsetof(D, position)}, {"viewport", offsetof(D, viewport)}, {"perspective", offsetof(D, perspective)},
                      {"raster", offsetof(D, raster)}, {"scissor", offsetof(NativeDrawSnapshot, scissor)}, {"known", offsetof(NativeDrawSnapshot, viewportKnown)}});
            } else if (differs[0] == 'p')
                Dump(expected.post, state.post, {{"post", 0}, {"known", offsetof(NativePostSnapshot, known)}});
            else if (differs[0] == 't') {
                std::vector<std::pair<const char*, size_t>> fields;
                static const char* units[] = {"tex0", "tex1", "tex2", "tex3", "tex4", "tex5", "tex6", "tex7"};
                for (unsigned i = 0; i < 8; i++) {
                    fields.push_back({units[i], offsetof(T, textures) + i * sizeof(B)});
                }
                fields.push_back({"palettes", offsetof(T, palettes)});
                fields.push_back({"valid", offsetof(T, valid)});
                Dump(expected.textures, state.textures, fields);
                std::fprintf(stderr, "  (binding: source %zu sampler@%zu map@%zu; source generation@%zu)\n", sizeof(mh3_native_texture_source), offsetof(B, sampler), offsetof(B, map_id),
                             offsetof(mh3_native_texture_source, generation));
            }
        }
        ++mismatches;
    }
    return true;
}
#endif

void ExecuteNativeModelInstance(uint32_t slot_index, uint32_t index) {
    auto& slot = slots.at(slot_index);
    const auto& instance = slot.instances.at(index);
    // Start from the game's copied blocks at this instance's capture.
    if (slot.mirrored != slot.sequence || !index) {
        slot.mirror = slot.base;
        slot.mirrored = slot.sequence;
    }
    for (size_t i = 0; i < instance.delta_count; i++) {
        const auto& delta = slot.deltas[instance.delta_first + i];
        std::memcpy(slot.mirror.data() + delta.address, slot.bytes.data() + delta.offset, delta.bytes);
    }
    slot.state = slot.mirror;
    regions.clear();
    for (size_t i = 0; i < slot.blocks.size(); i++)
        regions.push_back({slot.blocks[i].address, {slot.state.data() + slot.block_offset[i], slot.blocks[i].bytes}, 1});
    regions.push_back({ScratchBase, {slot.scratch.data(), ScratchBytes}, 1});
    auto pose = instance.pose;
    mh3_matrix_table* tables[3]{&pose.positions, &pose.normals, &pose.environment};
    for (unsigned i = 0; i < 3; i++) {
        if (!tables[i]->bytes.data || !tables[i]->bytes.size)
            continue;
        tables[i]->bytes.data = slot.bytes.data() + instance.pose_offset[i];
        if (instance.pose_address[i])
            regions.push_back({instance.pose_address[i], tables[i]->bytes, 0});
    }
    for (size_t i = 0; i < instance.run_count; i++) {
        const auto& run = slot.runs[instance.run_first + i];
        regions.push_back({run.address, {slot.bytes.data() + run.offset, run.bytes}, 0});
    }
    std::sort(regions.begin(), regions.end(), [](const mh3_res_region& a, const mh3_res_region& b) { return a.address < b.address; });
    for (size_t i = 1; i < regions.size(); i++)
        if (regions[i - 1].address + regions[i - 1].span.size > regions[i].address)
            throw std::runtime_error("Model instance regions overlap");
    auto view = MH3NativeResourceMemory();
    view.regions = regions.data();
    view.region_count = regions.size();
    {
        DrawMemory::Scope scope(&view);
        SetNativeModelInstancePose(instance.sources, &pose);
        drawing = Canonical(instance.owner);
        misses.clear();
#ifdef MH3_DEVELOPMENT
        if (validate) {
            checking = &instance;
            checkingSlot = &slot;
            checked = 0;
            mismatched = false;
            reported = 0;
            preparedOrder.clear();
        }
#endif
        try {
            // The light cache stays: its host light sets come along (their
            // slot assignment depends on the selections before).
            InvalidateNativeModelCaches(0x7ff & ~512u);
            SetNativeModelSelectors(instance.selectors);
            RestoreNativeLightingSnapshot(instance.lighting);
            SetNativeLightSets(instance.lightSets.data(), instance.lightSetCount);
            BeginNativeModelInstance();
            DrawNativeModelInstance(instance.owner, instance.options, instance.sources, ScratchBase);
            DrawNativeModelInstanceItems();
        } catch (const std::exception& error) {
            Report(instance.owner, error.what());
        }
#ifdef MH3_DEVELOPMENT
        if (validate && reported) {
            std::vector<const void*> expected;
            for (size_t i = 0; i < instance.item_count; i++)
                expected.push_back(slot.items[instance.item_first + i].prepared);
            auto actual = preparedOrder;
            const bool reordered = expected != actual;
            std::sort(expected.begin(), expected.end());
            std::sort(actual.begin(), actual.end());
            if (reordered)
                std::fprintf(stderr, "MH3 model instance %08x: items %s (%zu lines read live)\n", instance.owner, expected == actual ? "reordered" : "differ", misses.size());
        }
        if (validate && checked != instance.item_count && mismatches < 400) {
            std::fprintf(stderr, "MH3 model instance %08x: %zu of %zu items\n", instance.owner, checked, instance.item_count);
            ++mismatches;
        }
        checking = nullptr;
#endif
        drawing = 0;
        SetNativeModelInstancePose(nullptr, nullptr);
    }
    if (misses.empty())
        return;
    std::sort(misses.begin(), misses.end());
    misses.erase(std::unique(misses.begin(), misses.end()), misses.end());
    std::lock_guard lock(missLock);
    for (const uint32_t line : misses)
        missed.emplace_back(Canonical(instance.owner), line);
}
