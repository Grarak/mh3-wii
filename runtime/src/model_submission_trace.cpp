// The model draw walk (G3dDrawOpaXlu, 80078b00): a thin ABI adapter to the
// native C walk. It replaces 80078b00, not the downstream dispatch.
#include "abi_bridge.h"
#include "fatal.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "../native/mh3_model_draw.h"
#include "runtime/native/adapters/model_scope_bridge.h"

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
    constexpr uint32_t starts[] = {Memory::kMem1CachedBase, Memory::kMem1UncachedBase, Memory::kMem1PhysicalBase, Memory::kMem2CachedBase, Memory::kMem2UncachedBase, Memory::kMem2PhysicalBase};
    bool found = false;

    for (uint32_t start : starts) {
        const size_t size = (start == Memory::kMem1CachedBase || start == Memory::kMem1UncachedBase || start == Memory::kMem1PhysicalBase) ? Memory::kMem1Size : Memory::kMem2Size;

        if (view.resource < start || uint64_t(view.resource) - start >= size)
            continue;

        view.base = start;
        const mh3_res_span bank{Memory::GetPointer(start, size), size};
        const auto status = mh3_res_model_open(bank, view.resource - start, &view.model);

        if (status != MH3_RES_OK)
            MH3_THROW(std::runtime_error("Native model open failed: " + std::to_string(status)));

        if (cpu->gpr[4] < start)
            MH3_THROW(std::runtime_error("Native model bytecode is outside RAM bank"));

        view.bytecode = cpu->gpr[4] - start;
        found = true;
        break;
    }

    if (!found)
        MH3_THROW(std::runtime_error("Native model resource is outside RAM"));

    const uint32_t visibility = Memory::Read32(view.replacement + 4);

    if (visibility) {
        size_t dictionary;
        uint32_t count;
        const auto status = mh3_res_model_dictionary(&view.model, MH3_RES_NODES, &dictionary, &count);

        if (status != MH3_RES_OK)
            MH3_THROW(std::runtime_error("Native model node dictionary failed"));

        view.visibility = {Memory::GetPointer(visibility, count), count};
    }

    return view;
}

uint32_t Address(const ModelView& view, size_t offset) {
    return view.base + uint32_t(offset);
}

struct SubmitContext {
    CpuContext* cpu;
    const ModelView* view;
    uint32_t shape, node, material, swaps;
};

mh3_res_status Submit(void* opaque, const mh3_model_draw_item* item) {
    const auto& submit = *static_cast<SubmitContext*>(opaque);
    Memory::Write32(submit.shape, Address(*submit.view, item->shape.offset));
    Memory::Write32(submit.node, Address(*submit.view, item->node.offset));
    Memory::Write32(submit.material, Address(*submit.view, item->material.offset));
    mh3_model_material_bindings bindings;
    const mh3_res_span replacement{Memory::GetPointer(submit.view->replacement, 64), 64};
    const auto status = mh3_model_material_select(replacement, item->material_index, &bindings);

    if (status != MH3_RES_OK)
        return status;
    // The remaining guest dispatch consumes serialized reference wrappers.
    // Keep this endian/ABI conversion outside the native C selection logic.
    for (unsigned i = 0; i < 11; ++i)
        Memory::Write32(submit.swaps + i * 4, bindings.resource_slots[i]);

    for (unsigned i = 0; i < 3; ++i)
        Memory::Write32(submit.swaps + (i + 11) * 4, bindings.vertex_tables[i]);

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
    const auto status = mh3_model_draw_walk(&view.model, view.bytecode, view.visibility, options, Submit, &submit, nullptr);

    if (status != MH3_RES_OK)
        MH3_THROW(std::runtime_error("Native model draw failed: " + std::to_string(status)));
}

void RunDraw(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("G3dDrawOpaXlu")) {
        PortCheck::Run(cpu, "G3dDrawOpaXlu", func_80078B00, NativeDraw);
        return;
    }
#endif
    NativeDraw(cpu);
}

} // namespace

extern "C" void MH3_ModelDraw(CpuContext* cpu) {
    RunDraw(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80078B00, MH3_ModelDraw, "G3dDrawOpaXlu");

void TraceNativeModelDispatch(const NativeModelDispatch&) {
    // The model submission trace was removed; model_scope.cpp still calls this.
}
