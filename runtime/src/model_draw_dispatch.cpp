// Thin RMHP08 ABI adapters. Typed C owns draw order, resource selection and
// the authored sort; retained handlers and renderer callbacks stay at this seam.
#include "abi_bridge.h"
#include "fatal.h"
#include "guest_span.h"
#include "frame_split.h"
#include "guest_call.h"
#include "guest_worker.h"
#include "memory.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/adapters/model_scope_bridge.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_draw_dispatch.h"
#include <array>
#include <stdexcept>
#include <string>
#ifdef MH3_DEVELOPMENT
#include <cstdio>
#endif

const mh3_res_memory& MH3NativeResourceMemory();
#ifdef MH3_DEVELOPMENT
// native_resource_lifetime.cpp: the logic's frees and fills over [begin, end)
// beside the overlapped draw (MH3_TRACE=draw-writes).
void ReportDrawSideWrites(uint32_t begin, uint32_t end);
#endif
namespace {
constexpr uint32_t Phase = 0x807936b8;
std::array<mh3_model_draw_record, MH3_MODEL_SORT_CAPACITY> work;
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    MH3_THROW(std::runtime_error("Native model draw dispatch failed: " + std::to_string(status)));
}

inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}

mh3_res_span Bytes(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}

mh3_res_mut_span Mutable(uint32_t address, size_t size) {
    return {GuestRange(address, size), size};
}

// Guest words the draw passes by reference, in a guest stack frame.
struct References {
    uint32_t shape, node, material, bindings;
};

struct Model {
    uint32_t base, resource;
    mh3_res_model value;
    explicit Model(uint32_t address) : resource(address) {
        mh3_res_memory_view view{};
        Checked(mh3_res_runtime_map(&MH3NativeResourceMemory(), address, 16, &view));
        base = view.guest_base;
        Checked(mh3_res_model_open(view.span, view.offset, &value));
    }

    uint32_t Address(size_t offset) const {
        if (offset > UINT32_MAX - base)
            MH3_THROW(std::runtime_error("Model resource address overflow"));

        return base + uint32_t(offset);
    }

    size_t Stream(uint32_t address) const {
        mh3_res_memory_view view{};
        Checked(mh3_res_runtime_map(&MH3NativeResourceMemory(), address, 1, &view));

        if (view.span.data != value.container.data)
            MH3_THROW(std::runtime_error("Draw stream belongs to another bank"));

        return view.offset;
    }

    mh3_matrix_table Positions(uint32_t address) const {
        constexpr uint32_t offset = 0x68; // RMHP08 GetResMdlInfo + GetNumViewMtx
        if (value.size < offset + 4)
            MH3_THROW(std::runtime_error("Model matrix count is outside resource"));

        const uint32_t count = Memory::Read32(resource + offset);

        if (uint64_t(count) * 48 > uint64_t(SIZE_MAX))
            MH3_THROW(std::runtime_error("Model matrix allocation size overflow"));

        return {Bytes(address, size_t(count) * 48), count, 48, 1};
    }
};
#ifdef MH3_DEVELOPMENT
// MH3_DRAW_TABLE=2: the item's entries from the table are the dictionaries'.
void CompareEntries(const Model& model, const mh3_model_draw_item& item) {
    const struct {
        mh3_res_kind kind;
        uint32_t id;
        size_t offset;
        const char* name;
    } entries[]{{MH3_RES_MATERIALS, item.draw.material_id, item.material.offset, "material entry"},
                {MH3_RES_SHAPES, item.draw.shape_id, item.shape.offset, "shape entry"},
                {MH3_RES_NODES, item.draw.node_id, item.node.offset, "node entry"}};
    for (const auto& e : entries) {
        mh3_res_entry expected{};

        if (mh3_res_model_entry(&model.value, e.kind, e.id, &expected) != MH3_RES_OK || expected.offset != e.offset)
            ReportNativeModelDrawTableMismatch(e.name);
    }
}
#endif
struct Submit {
    const Model& model;
    uint32_t replacement;
    References refs;

    // The model's draw table, when it has one: items dispatch by ID.
    const NativeModelDrawTable* table;
    static mh3_res_status Emit(void* user, const mh3_model_draw_item* item) {
        auto& s = *static_cast<Submit*>(user);
        const auto& r = s.refs;
#ifdef MH3_DEVELOPMENT
        if (s.table && NativeModelDrawTableCompare())
            CompareEntries(s.model, *item);
#endif
        const uint32_t shape = s.model.Address(item->shape.offset), material = s.model.Address(item->material.offset);
        Memory::Write32(r.shape, shape);
        Memory::Write32(r.node, s.model.Address(item->node.offset));
        Memory::Write32(r.material, material);
        mh3_model_material_bindings replacements{};

        if (s.replacement) {
            const auto status = mh3_model_material_select(Bytes(s.replacement, 64), item->material_index, &replacements);

            if (status)
                return status;

            for (unsigned i = 0; i < 11; i++)
                Memory::Write32(r.bindings + i * 4, replacements.resource_slots[i]);

            for (unsigned i = 0; i < 3; i++)
                Memory::Write32(r.bindings + (i + 11) * 4, replacements.vertex_tables[i]);
        }

        const uint32_t bindings = s.replacement ? r.bindings : 0;

        // 80077814 and 80076A58 are the native ModelIndirectMatrices and
        // ModelDisplayListDispatch; neither reads guest registers or the
        // stack, so call them directly instead of through a guarded guest call.
        const uint32_t descriptor = NativeModelIndirectMatricesAt(material, shape, r.material, r.node, r.shape);
        const NativeModelDispatch dispatch{r.material, r.shape, 0, 0, item->dispatch_flags, bindings, descriptor};

        if (!s.table || !DispatchNativeModelIndexed(*s.table, item->draw.material_id, item->draw.shape_id, s.replacement ? &replacements : nullptr, dispatch))
            DispatchNativeModel(dispatch);

        return MH3_RES_OK;
    }
};

mh3_res_status ReadRecord(void* user, uint32_t index, mh3_model_draw_record* out) {
    const auto start = *static_cast<uint32_t*>(user);

    if (index > (UINT32_MAX - start) / 12)
        return MH3_RES_RANGE;

    return mh3_model_draw_record_read(Bytes(start + index * 12, 12), out);
}

void Draw(CpuContext* cpu, uint32_t resource, uint32_t stream, uint32_t count, uint32_t replacement, uint32_t options, bool sorted) {
    const Model model(resource);
    GuestFrame frame(cpu, 0x80);
    const References refs{frame[0x10], frame[0x14], frame[0x18], frame[0x20]};
    std::shared_ptr<NativeModelDrawTable> hold;
    const auto* table = NativeModelDrawTableFor(resource, hold);

    // A table also holds the draw stream's entries: no dictionary lookups.
    const mh3_model_draw_entries* entries = table ? NativeModelDrawEntries(*table) : nullptr;
    Submit submit{model, replacement, refs, table};
    BeginNativeModelDraw();

    if (sorted)
        Checked(mh3_model_draw_sorted_read_entries(&model.value, count, options, ReadRecord, &stream, entries, Submit::Emit, &submit, nullptr));
    else {
        mh3_res_span visibility{};

        if (replacement) {
            const uint32_t address = Memory::Read32(replacement + 4);

            if (address) {
                mh3_res_memory_view view{};
                Checked(mh3_res_runtime_map(&MH3NativeResourceMemory(), address, 1, &view));
                visibility = {view.span.data + view.offset, view.span.size - view.offset};
            }
        }

        Checked(mh3_model_draw_walk_entries(&model.value, model.Stream(stream), visibility, options, entries, Submit::Emit, &submit, nullptr));
    }

    EndNativeModelDraw();
}

uint32_t Collect(const Model& model, uint32_t position, uint32_t stream, uint32_t replacement) {
    const mh3_model_draw_collection input{
        &model.value, model.Stream(stream), &MH3NativeResourceMemory(), replacement ? Bytes(replacement, 64) : mh3_res_span{}, model.Positions(position), Memory::ReadFloat32(0x807982d8)};
    uint32_t count = 0;
    Checked(mh3_model_draw_collect(&input, work.data(), work.size(), &count));
    return count;
}

void Publish(uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        Checked(mh3_model_draw_record_write(Mutable(G3dScratch() + i * 12, 12), &work[i]));
}

struct Sort {
    CpuContext* cpu;
    uint32_t base, comparator;
    mh3_model_draw_record* records;
    const uint32_t* addresses = nullptr;
    uint32_t Address(size_t index) const { return addresses ? addresses[index] : base + uint32_t(index * 12); }
    static mh3_model_draw_record Record(const Sort& s, size_t index) {
        if (s.records)
            return s.records[index];

        mh3_model_draw_record record{};
        Checked(mh3_model_draw_record_read(Bytes(s.Address(index), 12), &record));
        return record;
    }

    static int Less(void* user, size_t a, size_t b) {
        auto& s = *static_cast<Sort*>(user);

        if (s.comparator == 0x80079f78 || s.comparator == 0x80079fdc) {
            const auto first = Record(s, a), second = Record(s, b);
            return mh3_model_draw_less(&first, &second, s.comparator == 0x80079f78);
        }

        if (!s.cpu)
            MH3_THROW(std::runtime_error("Model sort comparator has no native producer"));

        return GuestCall{s.cpu}(s.comparator, {s.Address(a), s.Address(b)}) != 0;
    }

    static void Swap(void* user, size_t a, size_t b) {
        auto& s = *static_cast<Sort*>(user);

        if (s.records)
            mh3_model_draw_swap(&s.records[a], &s.records[b]);
        else {
            auto first = Record(s, a), second = Record(s, b);
            mh3_model_draw_swap(&first, &second);
            Checked(mh3_model_draw_record_write(Mutable(s.Address(a), 12), &first));
            Checked(mh3_model_draw_record_write(Mutable(s.Address(b), 12), &second));
        }
    }

    static int32_t PhaseValue(void*, unsigned which) { return int32_t(Memory::Read32(Phase + which * 4)); }
    static void SetPhase(void*, unsigned which, int32_t value) { Memory::Write32(Phase + which * 4, uint32_t(value)); }
    mh3_model_sort_ops Ops() { return {this, Less, Swap, PhaseValue, SetPhase}; }
};

size_t Count(uint32_t begin, uint32_t end) {
    if (end < begin || (end - begin) % 12 || (end - begin) / 12 > MH3_MODEL_SORT_CAPACITY)
        MH3_THROW(std::runtime_error("Invalid model sort range"));

    if (end != begin)
        (void)Bytes(begin, end - begin);

    return (end - begin) / 12;
}

struct Pipeline {
    CpuContext* cpu;
    uint32_t reference, position;
    static void Bind(void*, const uint32_t sources[3]) { StoreNativeModelMatrixSources(0x80685530, sources); }
    // 80071c9c waits for HID2's locked-cache DMA count, then a system-call
    // ordering barrier. Native view producers finish synchronously and publish
    // completion before draw; they have no pending locked-cache DMA queue.
    static void Complete(void*) {}
    static mh3_res_status Pass(void* user, uint32_t stream, uint32_t replacement, uint32_t options, int sorted, int xlu) {
        auto& p = *static_cast<Pipeline*>(user);
        const uint32_t resource = Memory::Read32(p.reference);

        if (!sorted) {
            Draw(p.cpu, resource, stream, 0, replacement, options, false);
            return MH3_RES_OK;
        }

        const Model model(resource);
        const uint32_t count = Collect(model, p.position, stream, replacement);
        Sort sort{p.cpu, G3dScratch(), xlu ? 0x80079f78u : 0x80079fdcu, work.data()};
        auto ops = sort.Ops();
        Checked(mh3_model_draw_sort(&ops, 0, count, 0));
        Publish(count);

        // The original scratch is shared; callbacks must observe its exact
        // contents and later iterations must reread any nested draw mutation.
        Draw(p.cpu, Memory::Read32(p.reference), G3dScratch(), count, replacement, options, true);
        return MH3_RES_OK;
    }
};

void PipelineRun(CpuContext* cpu, uint32_t reference, const uint32_t sources[3], uint32_t opaque, uint32_t xlu, uint32_t replacement, uint32_t options) {
    Pipeline p{cpu, reference, sources[0]};
    const mh3_model_draw_pipeline ops{&p, Pipeline::Bind, Pipeline::Complete, Pipeline::Pass};
    Checked(mh3_model_draw_pipeline_run(&ops, sources, opaque, xlu, replacement, options));
}

struct Scene {
    GuestCall call;
    uint32_t options_word;
#ifdef MH3_DEVELOPMENT
    // A slow model draw's parts (MH3_ModelSceneDraw): task virtual, handler,
    // virtual, callback, source, draw pass; the slowest handler task. Timed
    // with MH3_PROFILE=split only: the clock reads and conversions of every
    // part took about 3% of the Vita's draw thread.
    double ms[6]{};
    unsigned slowTask = 0;
    double slowTaskMs = 0;
    struct Part {
        Scene& s;
        unsigned index;
        bool timed = mh3SplitDetail;
        MH3SplitClock::time_point start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};

        ~Part() {
            if (timed)
                s.ms[index] += std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();
        }
    };
#define MH3_SCENE_PART(scene, index) Scene::Part mh3ScenePart{scene, index}
#else
#define MH3_SCENE_PART(scene, index)
#endif
    mh3_model_scene_draw_ops Ops() { return {this, Task, Handler, Virtual, Callback, Options, Source, DrawPass}; }
    static void Task(void* user, uint32_t owner, unsigned task, uint32_t* options) {
        auto& s = *static_cast<Scene*>(user);
#ifdef MH3_DEVELOPMENT
        // A model whose draw virtual is gone was destroyed after the scene
        // was prepared: what wrote over it (MH3_TRACE=draw-writes).
        if (!Memory::Read32(Memory::Read32(owner) + 0x0c)) {
            std::fprintf(stderr, "MH3 draw of a dead model %08x (task %u):", owner, task);

            for (uint32_t i = 0; i < 16; ++i)
                std::fprintf(stderr, " %08x", Memory::Read32(owner + 4 * i));

            std::fprintf(stderr, "\n");
            ReportDrawSideWrites(owner, owner + 0x148);
        }
#endif
        MH3_SCENE_PART(s, 0);
        Memory::Write32(s.options_word, *options);
        s.call.Virtual<0x0c>(owner, {task, 0, s.options_word});
        *options = Memory::Read32(s.options_word);
    }

    static void Handler(void* user, uint32_t owner, uint32_t task, uint32_t argument, uint32_t options) {
        auto& s = *static_cast<Scene*>(user);
#ifdef MH3_DEVELOPMENT
        const bool timed = mh3SplitDetail;
        const auto start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};
        struct Slowest {
            Scene& s;
            uint32_t task;
            bool timed;
            MH3SplitClock::time_point start;
            ~Slowest() {
                if (!timed)
                    return;

                const double elapsed = std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count();

                if (task != 6 && task != 7)
                    s.ms[1] += elapsed;

                if (elapsed > s.slowTaskMs) {
                    s.slowTaskMs = elapsed;
                    s.slowTask = task;
                }
            }
        } slowest{s, task, timed, start};
#endif

        switch (task) {
        case 1:
            s.call.Call<0x8007D4E0>({owner, argument, options});
            break;
        case 2:
            s.call.Call<0x8007D600>({owner, argument, options});
            break;
        case 3:
            s.call.Call<0x8007DDF8>({owner, argument, options});
            break;
        case 4:
            s.call.Call<0x8007FADC>({owner, argument, options});
            break;
        case 5:
            s.call.Call<0x8007F90C>({owner, argument, options});
            break;
        case 6:
        case 7: {
            auto ops = s.Ops();
            Checked((task == 6 ? mh3_model_scene_opa : mh3_model_scene_xlu)(&ops, Bytes(owner, 0x148), owner, argument, options));
            break;
        }
        case 8:
            s.call.Call<0x8007DFFC>({owner, argument, options});
            break;
        default:
            s.call.Call<0x80081A94>({owner, task, argument, options});
            break;
        }
    }

    static void Virtual(void* user, uint32_t owner, unsigned slot, uint32_t argument) {
        auto& call = static_cast<Scene*>(user)->call;
        MH3_SCENE_PART(*static_cast<Scene*>(user), 2);

        if (slot == 0x24)
            call.Virtual<0x24>(owner);
        else if (slot == 0x38)
            call.Virtual<0x38>(owner, {argument});
        else
            MH3_THROW(std::runtime_error("Unexpected scene virtual slot"));
    }

    static void Callback(void* user, uint32_t callback, unsigned slot, unsigned mask, uint32_t owner, uint32_t argument, uint32_t options) {
        auto& call = static_cast<Scene*>(user)->call;
        MH3_SCENE_PART(*static_cast<Scene*>(user), 3);

        if (slot == 0x18)
            call.Virtual<0x18>(callback, {mask, owner, argument, options});
        else
            call.Virtual<0x1c>(callback, {mask, owner, argument, options});
    }

    static uint32_t Options(void*, uint32_t pointer) { return Memory::Read32(pointer); }
    // The view arrays' getters (0x80080BC0, 0x80080C0C, 0x80080C68): native.
    static uint32_t Source([[maybe_unused]] void* user, uint32_t owner, unsigned kind) {
        MH3_SCENE_PART(*static_cast<Scene*>(user), 4);

        if (kind > 2)
            MH3_THROW(std::runtime_error("Unexpected model source kind"));

        return NativeModelViewArray(owner, kind);
    }

    static mh3_res_status DrawPass(void* user, uint32_t resource, const uint32_t sources[3], uint32_t opaque, uint32_t xlu, uint32_t replacement, uint32_t options) {
        auto& s = *static_cast<Scene*>(user);
        MH3_SCENE_PART(s, 5);
        GuestFrame frame(s.call.cpu, 0x20);
        Memory::Write32(frame[0x10], resource);
        PipelineRun(s.call.cpu, frame[0x10], sources, opaque, xlu, replacement, options);
        return MH3_RES_OK;
    }
};
} // namespace

extern "C" void MH3_ModelSceneDraw(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Scene s{{cpu}, frame[0x10]};
    auto ops = s.Ops();
#ifdef MH3_DEVELOPMENT
    const bool timed = mh3SplitDetail;
    const auto start = timed ? MH3SplitClock::now() : MH3SplitClock::time_point{};
#endif
    Checked(mh3_model_scene_draw(&ops, Bytes(frame.Arg(3), 0x148), frame.Arg(3), frame.Arg(4)));
#ifdef MH3_DEVELOPMENT
    // One model's draw over 20 ms (a stall the split hides): where it went.
    if (const double total = timed ? std::chrono::duration<double, std::milli>(MH3SplitClock::now() - start).count() : 0; total >= 20)
        std::printf("MH3 slow model draw %08x: %.1f ms: task virtual %.1f, handlers %.1f (slowest task %u %.1f), virtual %.1f, callback %.1f, source %.1f, draw pass %.1f\n",
                    frame.Arg(3), total, s.ms[0], s.ms[1], s.slowTask, s.slowTaskMs, s.ms[2], s.ms[3], s.ms[4], s.ms[5]);
#endif
}
REGISTER_NATIVE_FUNCTION_AS(0x8004978C, MH3_ModelSceneDraw, "ModelSceneDraw");
extern "C" void MH3_ModelSceneOptions(CpuContext* cpu) {
    uint32_t options = 0;
    Checked(mh3_model_scene_options(Bytes(cpu->gpr[3], 0x11c), &options));
    cpu->gpr[3] = options;
}
REGISTER_NATIVE_FUNCTION_AS(0x80049810, MH3_ModelSceneOptions, "ModelSceneOptions");
#ifdef MH3_DEVELOPMENT
namespace {
unsigned SceneTaskFamily(uint32_t task) {
    switch (task) {
    case 1:
        return MH3_SPLIT_G3D_WORLD;
    case 2:
        return MH3_SPLIT_G3D_MATERIAL;
    case 3:
        return MH3_SPLIT_G3D_VERTEX;
    case 4:
        return MH3_SPLIT_G3D_VIEW;
    case 8:
        return MH3_SPLIT_G3D_UPDATE;
    default:
        return MH3_SPLIT_G3D_OTHER;
    }
}
} // namespace
#endif

extern "C" void MH3_ModelSceneTask(CpuContext* cpu) {
    MH3_SPLIT_DETAIL_SCOPE(SceneTaskFamily(cpu->gpr[4]));
    GuestFrame frame(cpu, 0x20);
    Scene s{{cpu}, frame[0x10]};
    auto ops = s.Ops();
    Checked(mh3_model_scene_task(&ops, Bytes(frame.Arg(3), 0x148), frame.Arg(3), frame.Arg(4), frame.Arg(5), frame.Arg(6)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007DE60, MH3_ModelSceneTask, "ModelSceneTask");
extern "C" void MH3_ModelSceneXlu(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Scene s{{cpu}, frame[0x10]};
    auto ops = s.Ops();
    Checked(mh3_model_scene_xlu(&ops, Bytes(frame.Arg(3), 0x148), frame.Arg(3), frame.Arg(4), frame.Arg(5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007DD1C, MH3_ModelSceneXlu, "ModelSceneXlu");
extern "C" void MH3_ModelSceneOpa(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Scene s{{cpu}, frame[0x10]};
    auto ops = s.Ops();
    Checked(mh3_model_scene_opa(&ops, Bytes(frame.Arg(3), 0x148), frame.Arg(3), frame.Arg(4), frame.Arg(5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007DC40, MH3_ModelSceneOpa, "ModelSceneOpa");
extern "C" void MH3_ModelDrawPipeline(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t sources[3] = {frame.Arg(4), frame.Arg(5), frame.Arg(6)};
    PipelineRun(cpu, frame.Arg(3), sources, frame.Arg(7), frame.Arg(8), frame.Arg(9), frame.Arg(10));
}
REGISTER_NATIVE_FUNCTION_AS(0x80079408, MH3_ModelDrawPipeline, "ModelDrawPipeline");
extern "C" void MH3_ModelDrawUnsorted(CpuContext* cpu) {
    const auto reference = cpu->gpr[3], stream = cpu->gpr[4], options = cpu->gpr[5];
    Draw(cpu, Memory::Read32(reference), stream, 0, 0, options, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x800786F0, MH3_ModelDrawUnsorted, "ModelDrawUnsorted");
extern "C" void MH3_ModelDrawSorted(CpuContext* cpu) {
    const auto reference = cpu->gpr[3], stream = cpu->gpr[4], count = cpu->gpr[5], options = cpu->gpr[6];
    Draw(cpu, Memory::Read32(reference), stream, count, 0, options, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80078EE0, MH3_ModelDrawSorted, "ModelDrawSorted");
extern "C" void MH3_ModelDrawSortedReplacement(CpuContext* cpu) {
    const auto reference = cpu->gpr[3], stream = cpu->gpr[4], count = cpu->gpr[5], replacement = cpu->gpr[6], options = cpu->gpr[7];
    Draw(cpu, Memory::Read32(reference), stream, count, replacement, options, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x8007907C, MH3_ModelDrawSortedReplacement, "ModelDrawSortedReplacement");
extern "C" void MH3_ModelDrawCollect(CpuContext* cpu) {
    const uint32_t output = cpu->gpr[3], reference = cpu->gpr[4], position = cpu->gpr[5], stream = cpu->gpr[6], replacement = cpu->gpr[7];
    const uint32_t count = Collect(Model(Memory::Read32(reference)), position, stream, replacement);
    Publish(count);
    Memory::Write32(output, count);
    cpu->gpr[3] = G3dScratch();
}
REGISTER_NATIVE_FUNCTION_AS(0x8007923C, MH3_ModelDrawCollect, "ModelDrawCollect");
extern "C" void MH3_ModelDrawSort(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Sort s{{cpu}, frame.Arg(3), frame.Arg(5), nullptr};
    auto ops = s.Ops();
    Checked(mh3_model_draw_sort(&ops, 0, Count(frame.Arg(3), frame.Arg(4)), 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x80079668, MH3_ModelDrawSort, "ModelDrawSort");
extern "C" void MH3_ModelDrawSortRecursive(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Sort s{{cpu}, frame.Arg(3), Memory::Read32(frame.Arg(5)), nullptr};
    auto ops = s.Ops();
    Checked(mh3_model_draw_sort(&ops, 0, Count(frame.Arg(3), frame.Arg(4)), 1));
}
REGISTER_NATIVE_FUNCTION_AS(0x80079B9C, MH3_ModelDrawSortRecursive, "ModelDrawSortRecursive");
extern "C" void MH3_ModelDrawSelectionSort(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    Sort s{{cpu}, frame.Arg(3), Memory::Read32(frame.Arg(5)), nullptr};
    auto ops = s.Ops();
    mh3_model_draw_selection_sort(&ops, 0, Count(frame.Arg(3), frame.Arg(4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x8007999C, MH3_ModelDrawSelectionSort, "ModelDrawSelectionSort");
extern "C" void MH3_ModelDrawMinimum(CpuContext* cpu) {
    uint32_t result;
    {
        GuestFrame frame(cpu, 0x20);
        Sort s{{cpu}, frame.Arg(3), Memory::Read32(frame.Arg(5)), nullptr};
        auto ops = s.Ops();
        result = frame.Arg(3) + uint32_t(mh3_model_draw_minimum(&ops, 0, Count(frame.Arg(3), frame.Arg(4))) * 12);
    }

    cpu->gpr[3] = result;
}
REGISTER_NATIVE_FUNCTION_AS(0x80079A20, MH3_ModelDrawMinimum, "ModelDrawMinimum");
extern "C" void MH3_ModelDrawMedian(CpuContext* cpu) {
    GuestFrame frame(cpu, 0x20);
    const uint32_t addresses[3] = {frame.Arg(3), frame.Arg(4), frame.Arg(5)};
    Sort s{{cpu}, 0, Memory::Read32(frame.Arg(6)), nullptr, addresses};
    auto ops = s.Ops();
    mh3_model_draw_median(&ops, 0, 1, 2);
}
REGISTER_NATIVE_FUNCTION_AS(0x80079AAC, MH3_ModelDrawMedian, "ModelDrawMedian");
extern "C" void MH3_ModelDrawSwap(CpuContext* cpu) {
    mh3_model_draw_record a{}, b{};
    const uint32_t first = cpu->gpr[3], second = cpu->gpr[4];
    Checked(mh3_model_draw_record_read(Bytes(first, 12), &a));
    Checked(mh3_model_draw_record_read(Bytes(second, 12), &b));
    mh3_model_draw_swap(&a, &b);
    Checked(mh3_model_draw_record_write(Mutable(first, 12), &a));
    Checked(mh3_model_draw_record_write(Mutable(second, 12), &b));
}
REGISTER_NATIVE_FUNCTION_AS(0x80079ED0, MH3_ModelDrawSwap, "ModelDrawSwap");
static void Compare(CpuContext* cpu, bool xlu) {
    mh3_model_draw_record a{}, b{};
    Checked(mh3_model_draw_record_read(Bytes(cpu->gpr[3], 12), &a));
    Checked(mh3_model_draw_record_read(Bytes(cpu->gpr[4], 12), &b));
    cpu->gpr[3] = uint32_t(mh3_model_draw_less(&a, &b, xlu));
}

extern "C" void MH3_ModelDrawCompareOpa(CpuContext* cpu) {
    Compare(cpu, false);
}
REGISTER_NATIVE_FUNCTION_AS(0x80079FDC, MH3_ModelDrawCompareOpa, "ModelDrawCompareOpa");
extern "C" void MH3_ModelDrawCompareXlu(CpuContext* cpu) {
    Compare(cpu, true);
}
REGISTER_NATIVE_FUNCTION_AS(0x80079F78, MH3_ModelDrawCompareXlu, "ModelDrawCompareXlu");
