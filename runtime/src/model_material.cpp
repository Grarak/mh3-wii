// Thin RMHP08 material ABI adapters. Native C owns field effects;
// platform mapping and typed backend publication remain at this boundary.
// All 14 verified wrappers form one coordinated family. No GX fallback.
#include "abi_bridge.h"
#include "memory.h"
#include "native_graphics.h"
#include "runtime/native/adapters/model_catalog_bridge.h"
#include "runtime/native/adapters/model_material_bridge.h"
#include "runtime/native/mh3_model_material_updates.h"
#include "runtime/native/mh3_material_state.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

uint32_t MH3NativeResReference(uint32_t reference);
namespace {
void Checked(mh3_res_status status) {
    if(status!=MH3_RES_OK) throw std::runtime_error("Native material producer failed: "+std::to_string(status));
}
uint32_t Data(CpuContext* cpu) {return MH3NativeResReference(cpu->gpr[3]);}
void Submit(CpuContext* cpu,mh3_model_material_block kind,uint32_t count=1) {
    if((kind==MH3_MODEL_MATERIAL_INDIRECT || kind==MH3_MODEL_MATERIAL_TEXGEN) && !count) return;
    uint32_t address=Data(cpu);
    if(kind==MH3_MODEL_MATERIAL_PIPELINE) {
        if(address>UINT32_MAX-32) throw std::runtime_error("Material pipeline address overflows");
        address+=32;
    }
    OverlayNativeModelMaterialGroup(ReadNativeLiveMaterialBlockView(address,kind).fields,kind,count);
}
void Copy(CpuContext* cpu,mh3_model_material_block kind) {
    const uint32_t destination=cpu->gpr[4];
    CopyNativeLiveMaterialBlock(destination,Data(cpu),kind);cpu->gpr[3]=destination;
}
template<class Producer> void Update(CpuContext* cpu,mh3_model_material_block kind,Producer producer) {
    auto block=ReadNativeLiveMaterialBlock(Data(cpu),kind);
    std::array<uint8_t,160> compatibility{};
    if(block.bytes>compatibility.size()) throw std::runtime_error("Invalid CPU material setter group size");
    auto* output=Memory::GetPointer(block.address,block.bytes);
    std::memcpy(compatibility.data(),output,block.bytes);
    Checked(producer(&block.fields,mh3_res_mut_span{compatibility.data(),block.bytes},NextNativeMaterialRevision()));
    // Arguments and CPU serialization are validated on temporary owned values.
    // A provenance failure leaves RAM and published typed fields unchanged.
    RecordNativeTypedMaterialWrite(block.address,block.bytes,kind);
    std::memcpy(output,compatibility.data(),block.bytes);PublishNativeLiveMaterialBlock(block);
}
std::array<uint8_t,4> Color(uint32_t address) {
    std::array<uint8_t,4> result{};std::memcpy(result.data(),Memory::GetPointer(address,4),4);return result;
}
}
extern "C" void MH3_MatPixelSubmit(CpuContext* cpu) {Submit(cpu,MH3_MODEL_MATERIAL_PIXEL);} // 80094480
REGISTER_NATIVE_FUNCTION_AS(0x80094480,MH3_MatPixelSubmit,"MatPixelSubmit");
extern "C" void MH3_MatColorSubmit(CpuContext* cpu) {Submit(cpu,MH3_MODEL_MATERIAL_COLORS);} // 8009454C
REGISTER_NATIVE_FUNCTION_AS(0x8009454C,MH3_MatColorSubmit,"MatColorSubmit");
extern "C" void MH3_MatIndirectSubmit(CpuContext* cpu) {Submit(cpu,MH3_MODEL_MATERIAL_INDIRECT,uint8_t(cpu->gpr[4]));} // 80096634
REGISTER_NATIVE_FUNCTION_AS(0x80096634,MH3_MatIndirectSubmit,"MatIndirectSubmit");
extern "C" void MH3_MatTexgenSubmit(CpuContext* cpu) {Submit(cpu,MH3_MODEL_MATERIAL_TEXGEN,uint8_t(cpu->gpr[4]));} // 80097210
REGISTER_NATIVE_FUNCTION_AS(0x80097210,MH3_MatTexgenSubmit,"MatTexgenSubmit");
extern "C" void MH3_MatPipelineSubmit(CpuContext* cpu) {Submit(cpu,MH3_MODEL_MATERIAL_PIPELINE);} // 8009A244
REGISTER_NATIVE_FUNCTION_AS(0x8009A244,MH3_MatPipelineSubmit,"MatPipelineSubmit");
extern "C" void MH3_MatPixelCopy(CpuContext* cpu) {Copy(cpu,MH3_MODEL_MATERIAL_PIXEL);} // 800946D8
REGISTER_NATIVE_FUNCTION_AS(0x800946D8,MH3_MatPixelCopy,"MatPixelCopy");
extern "C" void MH3_MatColorCopy(CpuContext* cpu) {Copy(cpu,MH3_MODEL_MATERIAL_COLORS);} // 80094724
REGISTER_NATIVE_FUNCTION_AS(0x80094724,MH3_MatColorCopy,"MatColorCopy");
extern "C" void MH3_MatIndirectCopy(CpuContext* cpu) {Copy(cpu,MH3_MODEL_MATERIAL_INDIRECT);} // 80094770
REGISTER_NATIVE_FUNCTION_AS(0x80094770,MH3_MatIndirectCopy,"MatIndirectCopy");
extern "C" void MH3_MatTexgenCopy(CpuContext* cpu) {Copy(cpu,MH3_MODEL_MATERIAL_TEXGEN);} // 800947BC
REGISTER_NATIVE_FUNCTION_AS(0x800947BC,MH3_MatTexgenCopy,"MatTexgenCopy");
extern "C" void MH3_MatAlphaCompare(CpuContext* cpu) {
    Update(cpu,MH3_MODEL_MATERIAL_PIXEL,[&](auto* state,auto bytes,uint64_t revision) {
        return mh3_model_material_alpha_compare(state,bytes,cpu->gpr[4],uint8_t(cpu->gpr[5]),
            cpu->gpr[6],cpu->gpr[7],uint8_t(cpu->gpr[8]),revision);
    });
} // 80095F94
REGISTER_NATIVE_FUNCTION_AS(0x80095F94,MH3_MatAlphaCompare,"MatAlphaCompare");
extern "C" void MH3_MatBlend(CpuContext* cpu) {
    Update(cpu,MH3_MODEL_MATERIAL_PIXEL,[&](auto* state,auto bytes,uint64_t revision) {
        return mh3_model_material_blend(state,bytes,cpu->gpr[4],cpu->gpr[5],cpu->gpr[6],cpu->gpr[7],revision);
    });
} // 80096100
REGISTER_NATIVE_FUNCTION_AS(0x80096100,MH3_MatBlend,"MatBlend");
extern "C" void MH3_MatTevColor(CpuContext* cpu) {
    const auto color=Color(cpu->gpr[5]);
    Update(cpu,MH3_MODEL_MATERIAL_COLORS,[&](auto* state,auto bytes,uint64_t revision) {
        return mh3_model_material_tev_color(state,bytes,cpu->gpr[4],color.data(),revision);
    });
} // 80096300
REGISTER_NATIVE_FUNCTION_AS(0x80096300,MH3_MatTevColor,"MatTevColor");
extern "C" void MH3_MatKonstColor(CpuContext* cpu) {
    const auto color=Color(cpu->gpr[5]);
    Update(cpu,MH3_MODEL_MATERIAL_COLORS,[&](auto* state,auto bytes,uint64_t revision) {
        return mh3_model_material_konst_color(state,bytes,cpu->gpr[4],color.data(),revision);
    });
} // 80096548
REGISTER_NATIVE_FUNCTION_AS(0x80096548,MH3_MatKonstColor,"MatKonstColor");
extern "C" void MH3_MatIndirectMatrix(CpuContext* cpu) {
    std::array<float,12> matrix{};
    for(unsigned i=0;i<12;i++) matrix[i]=Memory::ReadFloat32(cpu->gpr[5]+i*4);
    Update(cpu,MH3_MODEL_MATERIAL_INDIRECT,[&](auto* state,auto bytes,uint64_t revision) {
        return mh3_model_material_indirect_matrix(state,bytes,cpu->gpr[4],matrix.data(),int8_t(cpu->gpr[6]),revision);
    });
} // 80096C10
REGISTER_NATIVE_FUNCTION_AS(0x80096C10,MH3_MatIndirectMatrix,"MatIndirectMatrix");
