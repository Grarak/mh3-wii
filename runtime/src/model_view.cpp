// Thin ABI/callback boundary for 80071CAC. The portable C producer performs
// typed CPU preparation; no GX, rendering fallback or anonymous pose marking.
#include "abi_bridge.h"
#include "guest_span.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_pose_bridge.h"
#include "runtime/native/mh3_model_view.h"
#include <cstring>
#include <stdexcept>
#include <string>

const mh3_res_memory& MH3NativeResourceMemory();
#ifdef MH3_DEVELOPMENT
void MH3FrameSplitCall(unsigned kind); // game_frame.cpp
#endif
namespace {
[[noreturn, gnu::cold, gnu::noinline]] void CheckedFailed(mh3_res_status status) {
    throw std::runtime_error("Native DMA view producer failed: " + std::to_string(status));
}
inline void Checked(mh3_res_status status) {
    if (status != MH3_RES_OK) [[unlikely]]
        CheckedFailed(status);
}
mh3_res_span Bytes(uint32_t address, size_t bytes) {
    return {GuestRange(address, bytes), bytes};
}
mh3_res_mut_span Mutable(uint32_t address, size_t bytes) {
    return {GuestRange(address, bytes), bytes};
}
size_t Aligned(size_t size) {
    return (size + 31) & ~size_t(31);
}
struct CallbackContext {
    CpuContext* cpu;
    uint32_t output[3], models, camera, reference, cache[2];
};
mh3_res_status BeforeStore(void* opaque, uint32_t kind, uint32_t first, size_t bytes) {
    const auto& context = *static_cast<CallbackContext*>(opaque);
    if (kind >= 3 || bytes > UINT32_MAX)
        return MH3_RES_INVALID;
    RetireNativeResourceOwner(context.output[kind] + first * (kind == 1 ? 36u : 48u), uint32_t(bytes), NativeSourceEpochProducer::ViewDma);
    return MH3_RES_OK;
}
mh3_res_status Billboard(void* opaque, uint32_t type, uint32_t id, int uniform, mh3_res_mut_span position) {
    auto& context = *static_cast<CallbackContext*>(opaque);
    // Known CPU billboard functions operate on the actual LC record. No draw
    // callback or translated CalcView path is invoked. Their port remains a
    // separate CPU math task; this callback preserves current behavior.
    if (type < 1 || type > 6 || position.size != 48)
        return MH3_RES_INVALID;
    const uint32_t destination = context.cache[(id / 40) & 1u] + (id % 40) * 48;
    if (GuestRange(destination, 48) != position.data)
        return MH3_RES_INVALID;
    GuestCall game{context.cpu};
    game(Memory::Read32(0x8056fdf8 + type * 4), {destination, context.models, uint32_t(uniform), context.camera, context.reference, id});
    return MH3_RES_OK;
}
void Produce(CpuContext* cpu) {
    const uint32_t count = cpu->gpr[7];
    if (!count)
        return;
    if (count > 65535u)
        throw std::runtime_error("Native DMA view count is outside scene allocation domain");
    const uint32_t position = cpu->gpr[3], normal = cpu->gpr[4], models = cpu->gpr[5], attributes = cpu->gpr[6];
    const uint32_t camera = cpu->gpr[8], reference = cpu->gpr[9], environment = cpu->gpr[10];
    if ((position | models | normal | environment) & 31u)
        throw std::runtime_error("Native DMA view arrays require authored 32-byte alignment");
    uint32_t resource = 0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(), reference, 1, &resource));
    mh3_res_memory_view bank{};
    Checked(mh3_res_runtime_map(&MH3NativeResourceMemory(), resource, 0x70, &bank));
    mh3_model_view_dma_input input{};
    Checked(mh3_res_model_open(bank.span, bank.offset, &input.model));
    if (Memory::Read32(resource + 0x68) != count)
        throw std::runtime_error("Native DMA count differs from declared model views");
    input.count = count;
    input.models = Bytes(models, Aligned(size_t(count) * 48));
    input.attributes = Bytes(attributes, size_t(count) * 4);
    input.position = Mutable(position, Aligned(size_t(count) * 48));
    if (normal)
        input.normal = Mutable(normal, Aligned(size_t(count) * 36));
    if (normal && environment)
        input.environment = Mutable(environment, Aligned(size_t(count) * 48));
    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 4; c++)
            input.camera.v[r][c] = Memory::ReadFloat32(camera + uint32_t(16 * r + 4 * c));
    // MH3's actual NW4R union ends at genmode 80685300: 2048 matrices,
    // 0x18000 bytes. Do not certify arbitrary following RAM as this scratch.
    input.billboard_work = Mutable(0x8066d300, 0x18000);
    CallbackContext context{cpu, {position, normal, environment}, models, camera, reference, {0xe0000800, 0xe0002800}};
    for (unsigned pair = 0; pair < 2; pair++) {
        const uint32_t cache = 0xe0000000u + pair * 0x2000;
        input.model_cache[pair] = Mutable(cache, 0x800);
        input.position_cache[pair] = Mutable(cache + 0x800, 0x800);
        input.normal_cache[pair] = Mutable(cache + 0x1000, 0x800);
        input.environment_cache[pair] = Mutable(cache + 0x1800, 0x800);
    }
    const mh3_model_view_callbacks callbacks{&context, Billboard, BeforeStore};
    Checked(mh3_model_view_dma(&input, &callbacks));
}
} // namespace
extern "C" void func_80071CAC(CpuContext*);
extern "C" void MH3_G3dCalcViewDma(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    MH3FrameSplitCall(3);
#endif
    const uint32_t position = cpu->gpr[3], normal = cpu->gpr[4], environment = cpu->gpr[10];
    const uint32_t count = cpu->gpr[7], reference = cpu->gpr[9];
#ifdef MH3_DEVELOPMENT
    if (PortCheck::Enabled("G3dCalcViewDma"))
        PortCheck::Run(cpu, "G3dCalcViewDma", func_80071CAC, Produce);
    else
#endif
        Produce(cpu);
    PublishNativeModelPose(position, normal, environment, count, reference);
}
REGISTER_NATIVE_FUNCTION_AS(0x80071CAC, MH3_G3dCalcViewDma, "G3dCalcViewDma");
