// Host GLES model submission validation and thin ABI adapter to native C.
// Rendering still uses the existing dispatch while its material/pose inputs
// are measured. The native walk replaces 80078b00, not the downstream GX path.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "../native/mh3_model_draw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void func_80078B00(CpuContext*);
extern "C" void func_80076A58(CpuContext*);

namespace {
struct ModelView {
    uint32_t base, resource, replacement;
    size_t bytecode;
    mh3_res_model model;
    mh3_res_span visibility;
};

ModelView View(CpuContext* cpu) {
    ModelView view{};
    view.resource = Memory::Read32(cpu->gpr[3]);
    view.replacement = cpu->gpr[5];
    // A BRRES string pool may be outside MDL0; retain the complete RAM bank.
    constexpr uint32_t starts[] = {Memory::kMem1CachedBase, Memory::kMem1UncachedBase,
        Memory::kMem1PhysicalBase, Memory::kMem2CachedBase,
        Memory::kMem2UncachedBase, Memory::kMem2PhysicalBase};
    bool found = false;
    for (uint32_t start : starts) {
        const size_t size = (start == Memory::kMem1CachedBase || start == Memory::kMem1UncachedBase ||
                             start == Memory::kMem1PhysicalBase) ? Memory::kMem1Size : Memory::kMem2Size;
        if (view.resource < start || uint64_t(view.resource) - start >= size) continue;
        view.base = start;
        const mh3_res_span bank{Memory::GetPointer(start, size), size};
        const auto status = mh3_res_model_open(bank, view.resource - start, &view.model);
        if (status != MH3_RES_OK) throw std::runtime_error("Native model open failed: " + std::to_string(status));
        if (cpu->gpr[4] < start) throw std::runtime_error("Native model bytecode is outside RAM bank");
        view.bytecode = cpu->gpr[4] - start;
        found = true;
        break;
    }
    if (!found) throw std::runtime_error("Native model resource is outside RAM");
    const uint32_t visibility = Memory::Read32(view.replacement + 4);
    if (visibility) {
        size_t dictionary;
        uint32_t count;
        const auto status = mh3_res_model_dictionary(&view.model, MH3_RES_NODES, &dictionary, &count);
        if (status != MH3_RES_OK) throw std::runtime_error("Native model node dictionary failed");
        view.visibility = {Memory::GetPointer(visibility, count), count};
    }
    return view;
}

uint32_t Address(const ModelView& view, size_t offset) { return view.base + uint32_t(offset); }

#ifdef MH3_DEVELOPMENT
struct TraceConfig {
    FILE* file = nullptr;
    uint32_t first = 0, last = UINT32_MAX;
    uint64_t calls = 0;
    TraceConfig() {
        const char* setting = std::getenv("MH3_MODEL_TRACE");
        if (!setting || !*setting) return;
        std::string path(setting);
        const auto comma = path.find(',');
        if (comma != std::string::npos) {
            if (std::sscanf(path.c_str() + comma, ",%u,%u", &first, &last) != 2 || last < first)
                throw std::runtime_error("MH3_MODEL_TRACE expects path,first,last");
            path.resize(comma);
        }
        file = std::fopen(path.c_str(), "w");
        if (!file) throw std::runtime_error("Could not open MH3_MODEL_TRACE");
        std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);
        std::fprintf(file, "# MH3 model submission trace v2; paced game frames; game-thread inputs\n");
    }
    ~TraceConfig() { if (file) std::fclose(file); }
};
TraceConfig& Config() { static TraceConfig config; return config; }
struct TraceScope {
    TraceScope* previous;
    ModelView view;
    std::vector<mh3_model_draw_item> expected;
    uint32_t frame, actual = 0, mismatches = 0;
    uint64_t call;
};
thread_local TraceScope* activeTrace = nullptr;

mh3_res_status Predict(void* opaque, const mh3_model_draw_item* item) {
    auto& trace = *static_cast<TraceScope*>(opaque);
    trace.expected.push_back(*item);
    std::fprintf(Config().file, "decision\t%u\t%llu\t%zu\t%u\t%u\t%u\t%u\t%08x\t%08x\t%08x\t%x\t%s\t%s\t%s\n",
        trace.frame, static_cast<unsigned long long>(trace.call), trace.expected.size() - 1,
        item->draw.material_id, item->draw.shape_id, item->draw.node_id, item->draw.priority,
        Address(trace.view, item->material.offset), Address(trace.view, item->shape.offset),
        Address(trace.view, item->node.offset), item->dispatch_flags,
        item->material.name, item->shape.name, item->node.name);
    return MH3_RES_OK;
}

void TraceDispatch(CpuContext* cpu) {
    if (!activeTrace) return;
    auto& trace = *activeTrace;
    const uint32_t material = Memory::Read32(cpu->gpr[3]);
    const uint32_t shape = Memory::Read32(cpu->gpr[4]);
    bool matches = trace.actual < trace.expected.size();
    if (matches) {
        const auto& item = trace.expected[trace.actual];
        matches = material == Address(trace.view, item.material.offset) &&
                  shape == Address(trace.view, item.shape.offset) && cpu->gpr[7] == item.dispatch_flags;
    }
    if (!matches) ++trace.mismatches;
    std::fprintf(Config().file, "dispatch\t%u\t%llu\t%u\t%08x\t%08x\t%x\t%08x\t%08x\t%08x\t%08x\t%u\n",
        trace.frame, static_cast<unsigned long long>(trace.call), trace.actual++, material, shape,
        cpu->gpr[7], cpu->gpr[5], cpu->gpr[6], cpu->gpr[8], cpu->gpr[9], unsigned(matches));
}
#endif

struct SubmitContext { CpuContext* cpu; const ModelView* view; uint32_t shape, node, material, swaps; };
mh3_res_status Submit(void* opaque, const mh3_model_draw_item* item) {
    const auto& submit = *static_cast<SubmitContext*>(opaque);
    Memory::Write32(submit.shape, Address(*submit.view, item->shape.offset));
    Memory::Write32(submit.node, Address(*submit.view, item->node.offset));
    Memory::Write32(submit.material, Address(*submit.view, item->material.offset));
    mh3_model_material_bindings bindings;
    const mh3_res_span replacement{Memory::GetPointer(submit.view->replacement, 64), 64};
    const auto status = mh3_model_material_select(replacement, item->material_index, &bindings);
    if (status != MH3_RES_OK) return status;
    // The remaining guest dispatch consumes serialized reference wrappers.
    // Keep this endian/ABI conversion outside the native C selection logic.
    for (unsigned i = 0; i < 11; ++i) Memory::Write32(submit.swaps + i * 4, bindings.resource_slots[i]);
    for (unsigned i = 0; i < 3; ++i) Memory::Write32(submit.swaps + (i + 11) * 4, bindings.vertex_tables[i]);
    GuestCall game{submit.cpu};
    const uint32_t descriptor = game.Call<0x80077814>({submit.material, submit.node, submit.shape});
    game.Call<0x80076A58>({submit.material, submit.shape, 0, 0, item->dispatch_flags, submit.swaps, descriptor});
    return MH3_RES_OK;
}

void NativeDraw(CpuContext* cpu) {
    const ModelView view = View(cpu);
    const uint32_t options = cpu->gpr[6];
    GuestFrame frame(cpu, 0x100);
    SubmitContext submit{cpu, &view, frame[0x10], frame[0x14], frame[0x18], frame[0x20]};
    const auto status = mh3_model_draw_walk(&view.model, view.bytecode, view.visibility,
                                           options, Submit, &submit, nullptr);
    if (status != MH3_RES_OK) throw std::runtime_error("Native model draw failed: " + std::to_string(status));
}

void RunDraw(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    static const bool native = [] {
        const char* setting = std::getenv("MH3_NATIVE_MODEL_DRAW");
        return !setting || setting[0] != '0';
    }();
    if (!native) { func_80078B00(cpu); return; }
    if (PortCheck::Enabled("G3dDrawOpaXlu")) {
        PortCheck::Run(cpu, "G3dDrawOpaXlu", func_80078B00, NativeDraw);
        return;
    }
#endif
    NativeDraw(cpu);
}

#ifdef MH3_DEVELOPMENT
const char* ModelName(const ModelView& view) {
    size_t at;
    const size_t field = view.model.offset + (view.model.version == 11 ? 0x48 : 0x3c);
    if (mh3_res_relative(view.model.container, view.model.offset, field, 1, &at) != MH3_RES_OK ||
        !std::memchr(view.model.container.data + at, 0, view.model.container.size - at))
        return "unknown";
    return reinterpret_cast<const char*>(view.model.container.data + at);
}
const char* BytecodeName(const ModelView& view) {
    size_t dictionary;
    uint32_t count;
    if (mh3_res_model_dictionary(&view.model, MH3_RES_BYTECODE, &dictionary, &count) != MH3_RES_OK)
        return "unknown";
    for (uint32_t i = 0; i < count; ++i) {
        mh3_res_entry entry;
        if (mh3_res_model_entry(&view.model, MH3_RES_BYTECODE, i, &entry) == MH3_RES_OK &&
            entry.offset == view.bytecode) return entry.name;
    }
    return "dynamic";
}
#endif
} // namespace

extern "C" void MH3_ModelDraw(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    auto& config = Config();
    const uint32_t frame = config.file ? Memory::Read32(cpu->gpr[13] - 0x45b8) : 0;
    if (config.file && frame >= config.first && frame <= config.last) {
        TraceScope trace{activeTrace, View(cpu), {}, frame, 0, 0, ++config.calls};
        const uint32_t options = cpu->gpr[6];
        std::fprintf(config.file, "model\t%u\t%llu\t%08x\t%08x\t%08x\t%x\t%zu\t%s\t%s\n", frame,
            static_cast<unsigned long long>(trace.call), trace.view.resource,
            Address(trace.view, trace.view.bytecode), trace.view.replacement, options, trace.view.visibility.size,
            ModelName(trace.view), BytecodeName(trace.view));
        mh3_model_draw_stats stats;
        const auto status = mh3_model_draw_walk(&trace.view.model, trace.view.bytecode,
            trace.view.visibility, options, Predict, &trace, &stats);
        if (status != MH3_RES_OK) throw std::runtime_error("Native model prediction failed: " + std::to_string(status));
        activeTrace = &trace;
        try { RunDraw(cpu); } catch (...) { activeTrace = trace.previous; throw; }
        activeTrace = trace.previous;
        if (trace.actual != trace.expected.size()) ++trace.mismatches;
        std::fprintf(config.file, "summary\t%u\t%llu\t%u\t%u\t%u\t%u\t%zu\t%u\t%u\n", frame,
            static_cast<unsigned long long>(trace.call), stats.records, stats.hidden_nodes,
            stats.hidden_shapes, stats.repeated_materials, trace.expected.size(), trace.actual, trace.mismatches);
        std::fflush(config.file);
        return;
    }
#endif
    RunDraw(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80078B00, MH3_ModelDraw, "G3dDrawOpaXlu");

void TraceNativeModelDispatch(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    TraceDispatch(cpu);
#endif
}
