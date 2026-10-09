// RMHP08 matrix ABI boundary. Matrix/resource logic is portable C; native
// records go directly to the renderer. No guest function, GX or FIFO calls.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_state_bridge.h"
#include "runtime/native/mh3_model_matrices.h"

#include <stdexcept>
#include <string>

const mh3_res_memory& MH3NativeResourceMemory();
// The renderer owns material scope and copies palette updates into GPU-owned
// storage before returning. A missing direct renderer is a link error.
void MH3NativeShapeSubmit(uint32_t shape_resource,const mh3_model_palette* palette,
                          const mh3_matrix_sources* sources);

namespace {
constexpr mh3_matrix_runtime_locations locations{
    0x80685530,0x80685510,0x80796db4,0x80688d94,0x80686368};
mh3_indirect_output indirect{};
void Checked(mh3_res_status status) {
    if (status!=MH3_RES_OK)
        throw std::runtime_error("Native model matrix input failed: "+std::to_string(status));
}
} // namespace

// Direct material preparation consumes these typed indirect matrices. The
// serialized object below only preserves the old caller's opaque return token.
const mh3_indirect_output* MH3NativeIndirectMatrices() { return &indirect; }
const mh3_matrix_binding* MH3NativeMatrixBinding() {
    static mh3_matrix_binding binding{};
    binding=ReadNativeModelMatrixSources(locations.arrays);return &binding;
}

extern "C" void MH3_ModelIndirectMatrices(CpuContext* cpu) {
    Checked(mh3_matrix_runtime_indirect(&MH3NativeResourceMemory(),&locations,
                                      cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],&indirect));
    if (!indirect.active) {cpu->gpr[3]=0;return;}
    uint32_t object=0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(),0x80793708,1,&object));
    // Captured standard operator: reset80088A5C / set800888BC. Custom virtual
    // operators need their own native implementation; do not invoke guest code.
    Checked(mh3_matrix_runtime_operator(&MH3NativeResourceMemory(),object,0x80088a5c,0x800888bc));
    Checked(mh3_matrix_store_indirect({Memory::GetPointer(object,152),152},&indirect));
    cpu->gpr[3]=object;
}
REGISTER_NATIVE_FUNCTION_AS(0x80077814,MH3_ModelIndirectMatrices,"ModelIndirectMatrices");

extern "C" void MH3_ModelShapePrimitive(CpuContext* cpu) {
    if (!NativeGraphicsFrameShouldRender()) return;
    mh3_matrix_runtime_storage storage{};
    mh3_shape_matrix_input input{};
    mh3_matrix_sources sources{};
    Checked(mh3_matrix_runtime_shape(&MH3NativeResourceMemory(),&locations,
                                   cpu->gpr[3],cpu->gpr[4],cpu->gpr[5],&storage,&input,&sources));
    if (!input.valid) return;
    sources.generation=ReadNativeModelMatrixSources(locations.arrays).generation;
    mh3_model_palette palette{};
    Checked(mh3_model_palette_rigid(&input,&sources,&palette));
    uint32_t shape=0;
    Checked(mh3_res_runtime_reference(&MH3NativeResourceMemory(),cpu->gpr[3],1,&shape));
    MH3NativeShapeSubmit(shape,&palette,&sources);
}
REGISTER_NATIVE_FUNCTION_AS(0x80087B34,MH3_ModelShapePrimitive,"ModelShapePrimitive");
