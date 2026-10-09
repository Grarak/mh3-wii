// Verified NW4R resource ABI adapters, RMHP08. Parsing and resource semantics
// live in native C; this file only maps owned RAM spans, arguments and results.
#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"
#include "../native/mh3_model_runtime.h"

#include <bit>
#include <cstdio>
#include <stdexcept>

namespace {
const mh3_res_memory& ResourceMemory() {
    static const mh3_res_memory memory{{Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size), Memory::kMem1Size},
                                       {Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size), Memory::kMem2Size},
                                       {Memory::GetPointer(0xe0000000, 0x4000), 0x4000}};
    return memory;
}
uint32_t Result(mh3_res_status status, uint32_t value) {
    if (status != MH3_RES_OK) {
        char message[96];
        std::snprintf(message, sizeof message, "Native model resource access failed (%d)", int(status));
        throw std::runtime_error(message);
    }
    return value;
}
} // namespace
const mh3_res_memory& MH3NativeResourceMemory() {
    return ResourceMemory();
}

// Thin shared adapter for the previously registered generic ResDic port.
uint32_t MH3NativeResDictionary(uint32_t reference, int32_t index) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_dictionary(&ResourceMemory(), reference, index, &value);
    return Result(status, value);
}

uint32_t MH3NativeResReference(uint32_t reference) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), reference, 1, &value);
    return Result(status, value);
}

extern "C" void func_80097F14(CpuContext*);
static void MdlNodePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_NODES, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNode(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlNode");
    if (check)
        PortCheck::RunPure(cpu, "MdlNode", PortCheck::Returns::Gpr3, func_80097F14, MdlNodePort);
    else
        MdlNodePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80097F14, MH3_MdlNode, "MdlNode");

extern "C" void func_8009802C(CpuContext*);
static void MdlPositionIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPositionInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlPositionInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlPositionInt", PortCheck::Returns::Gpr3, func_8009802C, MdlPositionIntPort);
    else
        MdlPositionIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009802C, MH3_MdlPositionInt, "MdlPositionInt");

extern "C" void func_80098094(CpuContext*);
static void MdlPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPosition(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlPosition");
    if (check)
        PortCheck::RunPure(cpu, "MdlPosition", PortCheck::Returns::Gpr3, func_80098094, MdlPositionPort);
    else
        MdlPositionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098094, MH3_MdlPosition, "MdlPosition");

extern "C" void func_80098144(CpuContext*);
static void MdlNormalIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormalInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlNormalInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlNormalInt", PortCheck::Returns::Gpr3, func_80098144, MdlNormalIntPort);
    else
        MdlNormalIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098144, MH3_MdlNormalInt, "MdlNormalInt");

extern "C" void func_800981AC(CpuContext*);
static void MdlNormalPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormal(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlNormal");
    if (check)
        PortCheck::RunPure(cpu, "MdlNormal", PortCheck::Returns::Gpr3, func_800981AC, MdlNormalPort);
    else
        MdlNormalPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800981AC, MH3_MdlNormal, "MdlNormal");

extern "C" void func_8009825C(CpuContext*);
static void MdlColorIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColorInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlColorInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlColorInt", PortCheck::Returns::Gpr3, func_8009825C, MdlColorIntPort);
    else
        MdlColorIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009825C, MH3_MdlColorInt, "MdlColorInt");

extern "C" void func_800982C4(CpuContext*);
static void MdlColorPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColor(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlColor");
    if (check)
        PortCheck::RunPure(cpu, "MdlColor", PortCheck::Returns::Gpr3, func_800982C4, MdlColorPort);
    else
        MdlColorPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800982C4, MH3_MdlColor, "MdlColor");

extern "C" void func_800983E0(CpuContext*);
static void MdlTexCoordIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoordInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlTexCoordInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlTexCoordInt", PortCheck::Returns::Gpr3, func_800983E0, MdlTexCoordIntPort);
    else
        MdlTexCoordIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800983E0, MH3_MdlTexCoordInt, "MdlTexCoordInt");

extern "C" void func_80098448(CpuContext*);
static void MdlTexCoordPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoord(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlTexCoord");
    if (check)
        PortCheck::RunPure(cpu, "MdlTexCoord", PortCheck::Returns::Gpr3, func_80098448, MdlTexCoordPort);
    else
        MdlTexCoordPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098448, MH3_MdlTexCoord, "MdlTexCoord");

extern "C" void func_80098680(CpuContext*);
static void MdlFurPositionIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPositionInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlFurPositionInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlFurPositionInt", PortCheck::Returns::Gpr3, func_80098680, MdlFurPositionIntPort);
    else
        MdlFurPositionIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098680, MH3_MdlFurPositionInt, "MdlFurPositionInt");

extern "C" void func_800986E8(CpuContext*);
static void MdlFurPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPosition(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlFurPosition");
    if (check)
        PortCheck::RunPure(cpu, "MdlFurPosition", PortCheck::Returns::Gpr3, func_800986E8, MdlFurPositionPort);
    else
        MdlFurPositionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800986E8, MH3_MdlFurPosition, "MdlFurPosition");

extern "C" void func_800987FC(CpuContext*);
static void MdlMaterialIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterialInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlMaterialInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlMaterialInt", PortCheck::Returns::Gpr3, func_800987FC, MdlMaterialIntPort);
    else
        MdlMaterialIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800987FC, MH3_MdlMaterialInt, "MdlMaterialInt");

extern "C" void func_80098864(CpuContext*);
static void MdlMaterialPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterial(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlMaterial");
    if (check)
        PortCheck::RunPure(cpu, "MdlMaterial", PortCheck::Returns::Gpr3, func_80098864, MdlMaterialPort);
    else
        MdlMaterialPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098864, MH3_MdlMaterial, "MdlMaterial");

extern "C" void func_80098914(CpuContext*);
static void MdlShapeIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShapeInt(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlShapeInt");
    if (check)
        PortCheck::RunPure(cpu, "MdlShapeInt", PortCheck::Returns::Gpr3, func_80098914, MdlShapeIntPort);
    else
        MdlShapeIntPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098914, MH3_MdlShapeInt, "MdlShapeInt");

extern "C" void func_8009897C(CpuContext*);
static void MdlShapePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShape(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlShape");
    if (check)
        PortCheck::RunPure(cpu, "MdlShape", PortCheck::Returns::Gpr3, func_8009897C, MdlShapePort);
    else
        MdlShapePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009897C, MH3_MdlShape, "MdlShape");

extern "C" void func_80097FE4(CpuContext*);
static void MdlNodeCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_NODES, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNodeCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlNodeCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlNodeCount", PortCheck::Returns::Gpr3, func_80097FE4, MdlNodeCountPort);
    else
        MdlNodeCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80097FE4, MH3_MdlNodeCount, "MdlNodeCount");

extern "C" void func_800980FC(CpuContext*);
static void MdlPositionCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPositionCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlPositionCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlPositionCount", PortCheck::Returns::Gpr3, func_800980FC, MdlPositionCountPort);
    else
        MdlPositionCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800980FC, MH3_MdlPositionCount, "MdlPositionCount");

extern "C" void func_80098214(CpuContext*);
static void MdlNormalCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormalCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlNormalCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlNormalCount", PortCheck::Returns::Gpr3, func_80098214, MdlNormalCountPort);
    else
        MdlNormalCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098214, MH3_MdlNormalCount, "MdlNormalCount");

extern "C" void func_8009832C(CpuContext*);
static void MdlColorCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColorCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlColorCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlColorCount", PortCheck::Returns::Gpr3, func_8009832C, MdlColorCountPort);
    else
        MdlColorCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009832C, MH3_MdlColorCount, "MdlColorCount");

extern "C" void func_800984B0(CpuContext*);
static void MdlTexCoordCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoordCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlTexCoordCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlTexCoordCount", PortCheck::Returns::Gpr3, func_800984B0, MdlTexCoordCountPort);
    else
        MdlTexCoordCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800984B0, MH3_MdlTexCoordCount, "MdlTexCoordCount");

extern "C" void func_800985CC(CpuContext*);
static void MdlFurVectorCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_VECTORS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurVectorCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlFurVectorCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlFurVectorCount", PortCheck::Returns::Gpr3, func_800985CC, MdlFurVectorCountPort);
    else
        MdlFurVectorCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800985CC, MH3_MdlFurVectorCount, "MdlFurVectorCount");

extern "C" void func_80098750(CpuContext*);
static void MdlFurPositionCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPositionCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlFurPositionCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlFurPositionCount", PortCheck::Returns::Gpr3, func_80098750, MdlFurPositionCountPort);
    else
        MdlFurPositionCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098750, MH3_MdlFurPositionCount, "MdlFurPositionCount");

extern "C" void func_800988CC(CpuContext*);
static void MdlMaterialCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterialCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlMaterialCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlMaterialCount", PortCheck::Returns::Gpr3, func_800988CC, MdlMaterialCountPort);
    else
        MdlMaterialCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800988CC, MH3_MdlMaterialCount, "MdlMaterialCount");

extern "C" void func_800989E4(CpuContext*);
static void MdlShapeCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShapeCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MdlShapeCount");
    if (check)
        PortCheck::RunPure(cpu, "MdlShapeCount", PortCheck::Returns::Gpr3, func_800989E4, MdlShapeCountPort);
    else
        MdlShapeCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800989E4, MH3_MdlShapeCount, "MdlShapeCount");

extern "C" void func_800773FC(CpuContext*);
static void ShpRefPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRef(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpRef");
    if (check)
        PortCheck::RunPure(cpu, "ShpRef", PortCheck::Returns::Gpr3, func_800773FC, ShpRefPort);
    else
        ShpRefPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800773FC, MH3_ShpRef, "ShpRef");

extern "C" void func_800776D8(CpuContext*);
static void ShpRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRefConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpRefConst");
    if (check)
        PortCheck::RunPure(cpu, "ShpRefConst", PortCheck::Returns::Gpr3, func_800776D8, ShpRefConstPort);
    else
        ShpRefConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800776D8, MH3_ShpRefConst, "ShpRefConst");

extern "C" void func_80077630(CpuContext*);
static void MatRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MatRefConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MatRefConst");
    if (check)
        PortCheck::RunPure(cpu, "MatRefConst", PortCheck::Returns::Gpr3, func_80077630, MatRefConstPort);
    else
        MatRefConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80077630, MH3_MatRefConst, "MatRefConst");

extern "C" void func_80099578(CpuContext*);
static void TagRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagRefConst");
    if (check)
        PortCheck::RunPure(cpu, "TagRefConst", PortCheck::Returns::Gpr3, func_80099578, TagRefConstPort);
    else
        TagRefConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099578, MH3_TagRefConst, "TagRefConst");

extern "C" void func_800997A4(CpuContext*);
static void TagRefPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRef(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagRef");
    if (check)
        PortCheck::RunPure(cpu, "TagRef", PortCheck::Returns::Gpr3, func_800997A4, TagRefPort);
    else
        TagRefPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800997A4, MH3_TagRef, "TagRef");

extern "C" void func_800696C8(CpuContext*);
static void ResDictionaryCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_dictionary_count(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ResDictionaryCount(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ResDictionaryCount");
    if (check)
        PortCheck::RunPure(cpu, "ResDictionaryCount", PortCheck::Returns::Gpr3, func_800696C8, ResDictionaryCountPort);
    else
        ResDictionaryCountPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800696C8, MH3_ResDictionaryCount, "ResDictionaryCount");

extern "C" void func_800776A8(CpuContext*);
static void ShpVisiblePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_visible(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpVisible(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpVisible");
    if (check)
        PortCheck::RunPure(cpu, "ShpVisible", PortCheck::Returns::Gpr3, func_800776A8, ShpVisiblePort);
    else
        ShpVisiblePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800776A8, MH3_ShpVisible, "ShpVisible");

extern "C" void func_80078968(CpuContext*);
static void NodeVisiblePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_node_visible(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_NodeVisible(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("NodeVisible");
    if (check)
        PortCheck::RunPure(cpu, "NodeVisible", PortCheck::Returns::Gpr3, func_80078968, NodeVisiblePort);
    else
        NodeVisiblePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80078968, MH3_NodeVisible, "NodeVisible");

extern "C" void func_8005D0B4(CpuContext*);
static void NodeIdPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 0x0c, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_NodeId(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("NodeId");
    if (check)
        PortCheck::RunPure(cpu, "NodeId", PortCheck::Returns::Gpr3, func_8005D0B4, NodeIdPort);
    else
        NodeIdPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005D0B4, MH3_NodeId, "NodeId");

extern "C" void func_80099464(CpuContext*);
static void ShpParentPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_parent(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpParent(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpParent");
    if (check)
        PortCheck::RunPure(cpu, "ShpParent", PortCheck::Returns::Gpr3, func_80099464, ShpParentPort);
    else
        ShpParentPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099464, MH3_ShpParent, "ShpParent");

extern "C" void func_800999D8(CpuContext*);
static void ShpPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPosition(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpPosition");
    if (check)
        PortCheck::RunPure(cpu, "ShpPosition", PortCheck::Returns::Gpr3, func_800999D8, ShpPositionPort);
    else
        ShpPositionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800999D8, MH3_ShpPosition, "ShpPosition");

extern "C" void func_80099A4C(CpuContext*);
static void ShpNormalPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpNormal(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpNormal");
    if (check)
        PortCheck::RunPure(cpu, "ShpNormal", PortCheck::Returns::Gpr3, func_80099A4C, ShpNormalPort);
    else
        ShpNormalPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099A4C, MH3_ShpNormal, "ShpNormal");

extern "C" void func_80099ABC(CpuContext*);
static void ShpColorPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_color(&ResourceMemory(), cpu->gpr[3], cpu->gpr[4], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpColor(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpColor");
    if (check)
        PortCheck::RunPure(cpu, "ShpColor", PortCheck::Returns::Gpr3, func_80099ABC, ShpColorPort);
    else
        ShpColorPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099ABC, MH3_ShpColor, "ShpColor");

extern "C" void func_80099B74(CpuContext*);
static void ShpTexCoordPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_texcoord(&ResourceMemory(), cpu->gpr[3], cpu->gpr[4], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpTexCoord(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpTexCoord");
    if (check)
        PortCheck::RunPure(cpu, "ShpTexCoord", PortCheck::Returns::Gpr3, func_80099B74, ShpTexCoordPort);
    else
        ShpTexCoordPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099B74, MH3_ShpTexCoord, "ShpTexCoord");

extern "C" void func_80099C14(CpuContext*);
static void ShpFurPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 13, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpFurPosition(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpFurPosition");
    if (check)
        PortCheck::RunPure(cpu, "ShpFurPosition", PortCheck::Returns::Gpr3, func_80099C14, ShpFurPositionPort);
    else
        ShpFurPositionPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099C14, MH3_ShpFurPosition, "ShpFurPosition");

extern "C" void func_80099604(CpuContext*);
static void ShpSetupTagConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 0, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupTagConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpSetupTagConst");
    if (check)
        PortCheck::RunPure(cpu, "ShpSetupTagConst", PortCheck::Returns::Gpr3, func_80099604, ShpSetupTagConstPort);
    else
        ShpSetupTagConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099604, MH3_ShpSetupTagConst, "ShpSetupTagConst");

extern "C" void func_80099810(CpuContext*);
static void ShpSetupTagPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 0, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupTag(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpSetupTag");
    if (check)
        PortCheck::RunPure(cpu, "ShpSetupTag", PortCheck::Returns::Gpr3, func_80099810, ShpSetupTagPort);
    else
        ShpSetupTagPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099810, MH3_ShpSetupTag, "ShpSetupTag");

extern "C" void func_8009A15C(CpuContext*);
static void ShpPrimitiveTagConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 1, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPrimitiveTagConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpPrimitiveTagConst");
    if (check)
        PortCheck::RunPure(cpu, "ShpPrimitiveTagConst", PortCheck::Returns::Gpr3, func_8009A15C, ShpPrimitiveTagConstPort);
    else
        ShpPrimitiveTagConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009A15C, MH3_ShpPrimitiveTagConst, "ShpPrimitiveTagConst");

extern "C" void func_80099E5C(CpuContext*);
static void ShpPrimitiveTagPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 1, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPrimitiveTag(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpPrimitiveTag");
    if (check)
        PortCheck::RunPure(cpu, "ShpPrimitiveTag", PortCheck::Returns::Gpr3, func_80099E5C, ShpPrimitiveTagPort);
    else
        ShpPrimitiveTagPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099E5C, MH3_ShpPrimitiveTag, "ShpPrimitiveTag");

extern "C" void func_8009974C(CpuContext*);
static void TagDisplayListPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_tag_list(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagDisplayList(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagDisplayList");
    if (check)
        PortCheck::RunPure(cpu, "TagDisplayList", PortCheck::Returns::Gpr3, func_8009974C, TagDisplayListPort);
    else
        TagDisplayListPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009974C, MH3_TagDisplayList, "TagDisplayList");

extern "C" void func_80099710(CpuContext*);
static void ShpSetupDisplayListPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_setup(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupDisplayList(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpSetupDisplayList");
    if (check)
        PortCheck::RunPure(cpu, "ShpSetupDisplayList", PortCheck::Returns::Gpr3, func_80099710, ShpSetupDisplayListPort);
    else
        ShpSetupDisplayListPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099710, MH3_ShpSetupDisplayList, "ShpSetupDisplayList");

extern "C" void func_80099E38(CpuContext*);
static void TagBufferSizePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagBufferSize(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagBufferSize");
    if (check)
        PortCheck::RunPure(cpu, "TagBufferSize", PortCheck::Returns::Gpr3, func_80099E38, TagBufferSizePort);
    else
        TagBufferSizePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099E38, MH3_TagBufferSize, "TagBufferSize");

extern "C" void func_8009A07C(CpuContext*);
static void TagCommandSizePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 4, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagCommandSize(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagCommandSize");
    if (check)
        PortCheck::RunPure(cpu, "TagCommandSize", PortCheck::Returns::Gpr3, func_8009A07C, TagCommandSizePort);
    else
        TagCommandSizePort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009A07C, MH3_TagCommandSize, "TagCommandSize");

extern "C" void func_800994A0(CpuContext*);
static void ShpRelativeModelPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_offset(&ResourceMemory(), cpu->gpr[3], std::bit_cast<int32_t>(cpu->gpr[4]), 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRelativeModel(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ShpRelativeModel");
    if (check)
        PortCheck::RunPure(cpu, "ShpRelativeModel", PortCheck::Returns::Gpr3, func_800994A0, ShpRelativeModelPort);
    else
        ShpRelativeModelPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800994A0, MH3_ShpRelativeModel, "ShpRelativeModel");

extern "C" void func_80099788(CpuContext*);
static void TagRelativeBytesPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_offset(&ResourceMemory(), cpu->gpr[3], std::bit_cast<int32_t>(cpu->gpr[4]), 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRelativeBytes(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagRelativeBytes");
    if (check)
        PortCheck::RunPure(cpu, "TagRelativeBytes", PortCheck::Returns::Gpr3, func_80099788, TagRelativeBytesPort);
    else
        TagRelativeBytesPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099788, MH3_TagRelativeBytes, "TagRelativeBytes");

extern "C" void func_80062918(CpuContext*);
static void ResDictionaryValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ResDictionaryValid(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("ResDictionaryValid");
    if (check)
        PortCheck::RunPure(cpu, "ResDictionaryValid", PortCheck::Returns::Gpr3, func_80062918, ResDictionaryValidPort);
    else
        ResDictionaryValidPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80062918, MH3_ResDictionaryValid, "ResDictionaryValid");

extern "C" void func_80077744(CpuContext*);
static void MatValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MatValid(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("MatValid");
    if (check)
        PortCheck::RunPure(cpu, "MatValid", PortCheck::Returns::Gpr3, func_80077744, MatValidPort);
    else
        MatValidPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80077744, MH3_MatValid, "MatValid");

extern "C" void func_800995F0(CpuContext*);
static void TagValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagValid(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagValid");
    if (check)
        PortCheck::RunPure(cpu, "TagValid", PortCheck::Returns::Gpr3, func_800995F0, TagValidPort);
    else
        TagValidPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800995F0, MH3_TagValid, "TagValid");

extern "C" void func_800995DC(CpuContext*);
static void TagRefUncheckedConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefUncheckedConst(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagRefUncheckedConst");
    if (check)
        PortCheck::RunPure(cpu, "TagRefUncheckedConst", PortCheck::Returns::Gpr3, func_800995DC, TagRefUncheckedConstPort);
    else
        TagRefUncheckedConstPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x800995DC, MH3_TagRefUncheckedConst, "TagRefUncheckedConst");

extern "C" void func_80099808(CpuContext*);
static void TagRefUncheckedPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefUnchecked(CpuContext* cpu) {
    static const bool check = PortCheck::Enabled("TagRefUnchecked");
    if (check)
        PortCheck::RunPure(cpu, "TagRefUnchecked", PortCheck::Returns::Gpr3, func_80099808, TagRefUncheckedPort);
    else
        TagRefUncheckedPort(cpu);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099808, MH3_TagRefUnchecked, "TagRefUnchecked");
