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
const mh3_res_memory& LiveMemory() {
    static const mh3_res_memory memory{{Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size), Memory::kMem1Size},
                                       {Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size), Memory::kMem2Size},
                                       {Memory::GetPointer(0xe0000000, 0x4000), 0x4000}};
    return memory;
}
const mh3_res_memory& ResourceMemory() {
    return LiveMemory();
}
uint32_t Result(mh3_res_status status, uint32_t value) {
    if (status != MH3_RES_OK) {
        char message[96];
        std::snprintf(message, sizeof message, "Native model resource access failed (%d)", int(status));
        throw std::runtime_error(message);
    }
    return value;
}

// An accessor's result from its argument registers. The registered bodies name
// only the registers they use (r1 for the check's original), so the translator
// calls them directly as precise natives, without the dispatch's register
// guard; the port runs on a local context the compiler folds away. With
// MH3_PORT_CHECK naming it (development builds), the original and the port
// run in the current context and compare r3.
template <void (*Original)(CpuContext*), void (*Port)(CpuContext*)>
uint32_t Accessor(const char* name, uint32_t r1, uint32_t r3, const uint32_t* r4 = nullptr) {
#ifdef MH3_DEVELOPMENT
    static const bool check = PortCheck::Enabled(name);
    if (check) [[unlikely]] {
        CpuContext* cpu = TryGetCpuContext();
        cpu->gpr[1] = r1;
        cpu->gpr[3] = r3;
        if (r4)
            cpu->gpr[4] = *r4;
        PortCheck::RunPure(cpu, name, PortCheck::Returns::Gpr3, Original, Port);
        return cpu->gpr[3];
    }
#else
    (void)name, (void)r1;
#endif
    CpuContext local;
    local.gpr[3] = r3;
    local.gpr[4] = r4 ? *r4 : 0;
    Port(&local);
    return local.gpr[3];
}
template <void (*Original)(CpuContext*), void (*Port)(CpuContext*)>
uint32_t Accessor(const char* name, uint32_t r1, uint32_t r3, uint32_t r4) {
    return Accessor<Original, Port>(name, r1, r3, &r4);
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
    cpu->gpr[3] = Accessor<func_80097F14, MdlNodePort>("MdlNode", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80097F14, MH3_MdlNode, "MdlNode");

extern "C" void func_8009802C(CpuContext*);
static void MdlPositionIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPositionInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009802C, MdlPositionIntPort>("MdlPositionInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009802C, MH3_MdlPositionInt, "MdlPositionInt");

extern "C" void func_80098094(CpuContext*);
static void MdlPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPosition(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098094, MdlPositionPort>("MdlPosition", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098094, MH3_MdlPosition, "MdlPosition");

extern "C" void func_80098144(CpuContext*);
static void MdlNormalIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormalInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098144, MdlNormalIntPort>("MdlNormalInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098144, MH3_MdlNormalInt, "MdlNormalInt");

extern "C" void func_800981AC(CpuContext*);
static void MdlNormalPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormal(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800981AC, MdlNormalPort>("MdlNormal", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800981AC, MH3_MdlNormal, "MdlNormal");

extern "C" void func_8009825C(CpuContext*);
static void MdlColorIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColorInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009825C, MdlColorIntPort>("MdlColorInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009825C, MH3_MdlColorInt, "MdlColorInt");

extern "C" void func_800982C4(CpuContext*);
static void MdlColorPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColor(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800982C4, MdlColorPort>("MdlColor", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800982C4, MH3_MdlColor, "MdlColor");

extern "C" void func_800983E0(CpuContext*);
static void MdlTexCoordIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoordInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800983E0, MdlTexCoordIntPort>("MdlTexCoordInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800983E0, MH3_MdlTexCoordInt, "MdlTexCoordInt");

extern "C" void func_80098448(CpuContext*);
static void MdlTexCoordPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoord(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098448, MdlTexCoordPort>("MdlTexCoord", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098448, MH3_MdlTexCoord, "MdlTexCoord");

extern "C" void func_80098680(CpuContext*);
static void MdlFurPositionIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPositionInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098680, MdlFurPositionIntPort>("MdlFurPositionInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098680, MH3_MdlFurPositionInt, "MdlFurPositionInt");

extern "C" void func_800986E8(CpuContext*);
static void MdlFurPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPosition(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800986E8, MdlFurPositionPort>("MdlFurPosition", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800986E8, MH3_MdlFurPosition, "MdlFurPosition");

extern "C" void func_800987FC(CpuContext*);
static void MdlMaterialIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterialInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800987FC, MdlMaterialIntPort>("MdlMaterialInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800987FC, MH3_MdlMaterialInt, "MdlMaterialInt");

extern "C" void func_80098864(CpuContext*);
static void MdlMaterialPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterial(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098864, MdlMaterialPort>("MdlMaterial", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098864, MH3_MdlMaterial, "MdlMaterial");

extern "C" void func_80098914(CpuContext*);
static void MdlShapeIntPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShapeInt(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098914, MdlShapeIntPort>("MdlShapeInt", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098914, MH3_MdlShapeInt, "MdlShapeInt");

extern "C" void func_8009897C(CpuContext*);
static void MdlShapePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, std::bit_cast<int32_t>(cpu->gpr[4]), &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShape(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009897C, MdlShapePort>("MdlShape", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009897C, MH3_MdlShape, "MdlShape");

extern "C" void func_80097FE4(CpuContext*);
static void MdlNodeCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_NODES, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNodeCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80097FE4, MdlNodeCountPort>("MdlNodeCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80097FE4, MH3_MdlNodeCount, "MdlNodeCount");

extern "C" void func_800980FC(CpuContext*);
static void MdlPositionCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_POSITIONS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlPositionCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800980FC, MdlPositionCountPort>("MdlPositionCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800980FC, MH3_MdlPositionCount, "MdlPositionCount");

extern "C" void func_80098214(CpuContext*);
static void MdlNormalCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_NORMALS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlNormalCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098214, MdlNormalCountPort>("MdlNormalCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098214, MH3_MdlNormalCount, "MdlNormalCount");

extern "C" void func_8009832C(CpuContext*);
static void MdlColorCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_COLORS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlColorCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009832C, MdlColorCountPort>("MdlColorCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009832C, MH3_MdlColorCount, "MdlColorCount");

extern "C" void func_800984B0(CpuContext*);
static void MdlTexCoordCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_TEXCOORDS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlTexCoordCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800984B0, MdlTexCoordCountPort>("MdlTexCoordCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800984B0, MH3_MdlTexCoordCount, "MdlTexCoordCount");

extern "C" void func_800985CC(CpuContext*);
static void MdlFurVectorCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_VECTORS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurVectorCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800985CC, MdlFurVectorCountPort>("MdlFurVectorCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800985CC, MH3_MdlFurVectorCount, "MdlFurVectorCount");

extern "C" void func_80098750(CpuContext*);
static void MdlFurPositionCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_FUR_POSITIONS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlFurPositionCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80098750, MdlFurPositionCountPort>("MdlFurPositionCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80098750, MH3_MdlFurPositionCount, "MdlFurPositionCount");

extern "C" void func_800988CC(CpuContext*);
static void MdlMaterialCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_MATERIALS, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlMaterialCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800988CC, MdlMaterialCountPort>("MdlMaterialCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800988CC, MH3_MdlMaterialCount, "MdlMaterialCount");

extern "C" void func_800989E4(CpuContext*);
static void MdlShapeCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_model_count(&ResourceMemory(), cpu->gpr[3], MH3_RES_SHAPES, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MdlShapeCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800989E4, MdlShapeCountPort>("MdlShapeCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800989E4, MH3_MdlShapeCount, "MdlShapeCount");

extern "C" void func_800773FC(CpuContext*);
static void ShpRefPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRef(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800773FC, ShpRefPort>("ShpRef", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800773FC, MH3_ShpRef, "ShpRef");

extern "C" void func_800776D8(CpuContext*);
static void ShpRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRefConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800776D8, ShpRefConstPort>("ShpRefConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800776D8, MH3_ShpRefConst, "ShpRefConst");

extern "C" void func_80077630(CpuContext*);
static void MatRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MatRefConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80077630, MatRefConstPort>("MatRefConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80077630, MH3_MatRefConst, "MatRefConst");

extern "C" void func_80099578(CpuContext*);
static void TagRefConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099578, TagRefConstPort>("TagRefConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099578, MH3_TagRefConst, "TagRefConst");

extern "C" void func_800997A4(CpuContext*);
static void TagRefPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRef(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800997A4, TagRefPort>("TagRef", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800997A4, MH3_TagRef, "TagRef");

extern "C" void func_800696C8(CpuContext*);
static void ResDictionaryCountPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_dictionary_count(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ResDictionaryCount(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800696C8, ResDictionaryCountPort>("ResDictionaryCount", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800696C8, MH3_ResDictionaryCount, "ResDictionaryCount");

extern "C" void func_800776A8(CpuContext*);
static void ShpVisiblePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_visible(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpVisible(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800776A8, ShpVisiblePort>("ShpVisible", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800776A8, MH3_ShpVisible, "ShpVisible");

extern "C" void func_80078968(CpuContext*);
static void NodeVisiblePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_node_visible(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_NodeVisible(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80078968, NodeVisiblePort>("NodeVisible", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80078968, MH3_NodeVisible, "NodeVisible");

extern "C" void func_8005D0B4(CpuContext*);
static void NodeIdPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 0x0c, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_NodeId(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8005D0B4, NodeIdPort>("NodeId", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8005D0B4, MH3_NodeId, "NodeId");

extern "C" void func_80099464(CpuContext*);
static void ShpParentPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_parent(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpParent(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099464, ShpParentPort>("ShpParent", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099464, MH3_ShpParent, "ShpParent");

extern "C" void func_800999D8(CpuContext*);
static void ShpPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPosition(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800999D8, ShpPositionPort>("ShpPosition", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800999D8, MH3_ShpPosition, "ShpPosition");

extern "C" void func_80099A4C(CpuContext*);
static void ShpNormalPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpNormal(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099A4C, ShpNormalPort>("ShpNormal", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099A4C, MH3_ShpNormal, "ShpNormal");

extern "C" void func_80099ABC(CpuContext*);
static void ShpColorPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_color(&ResourceMemory(), cpu->gpr[3], cpu->gpr[4], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpColor(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099ABC, ShpColorPort>("ShpColor", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099ABC, MH3_ShpColor, "ShpColor");

extern "C" void func_80099B74(CpuContext*);
static void ShpTexCoordPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_texcoord(&ResourceMemory(), cpu->gpr[3], cpu->gpr[4], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpTexCoord(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099B74, ShpTexCoordPort>("ShpTexCoord", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099B74, MH3_ShpTexCoord, "ShpTexCoord");

extern "C" void func_80099C14(CpuContext*);
static void ShpFurPositionPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_array(&ResourceMemory(), cpu->gpr[3], 13, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpFurPosition(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099C14, ShpFurPositionPort>("ShpFurPosition", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099C14, MH3_ShpFurPosition, "ShpFurPosition");

extern "C" void func_80099604(CpuContext*);
static void ShpSetupTagConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 0, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupTagConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099604, ShpSetupTagConstPort>("ShpSetupTagConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099604, MH3_ShpSetupTagConst, "ShpSetupTagConst");

extern "C" void func_80099810(CpuContext*);
static void ShpSetupTagPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 0, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupTag(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099810, ShpSetupTagPort>("ShpSetupTag", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099810, MH3_ShpSetupTag, "ShpSetupTag");

extern "C" void func_8009A15C(CpuContext*);
static void ShpPrimitiveTagConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 1, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPrimitiveTagConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009A15C, ShpPrimitiveTagConstPort>("ShpPrimitiveTagConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009A15C, MH3_ShpPrimitiveTagConst, "ShpPrimitiveTagConst");

extern "C" void func_80099E5C(CpuContext*);
static void ShpPrimitiveTagPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_tag(&ResourceMemory(), cpu->gpr[3], 1, 1, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpPrimitiveTag(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099E5C, ShpPrimitiveTagPort>("ShpPrimitiveTag", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099E5C, MH3_ShpPrimitiveTag, "ShpPrimitiveTag");

extern "C" void func_8009974C(CpuContext*);
static void TagDisplayListPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_tag_list(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagDisplayList(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009974C, TagDisplayListPort>("TagDisplayList", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009974C, MH3_TagDisplayList, "TagDisplayList");

extern "C" void func_80099710(CpuContext*);
static void ShpSetupDisplayListPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_shape_setup(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpSetupDisplayList(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099710, ShpSetupDisplayListPort>("ShpSetupDisplayList", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099710, MH3_ShpSetupDisplayList, "ShpSetupDisplayList");

extern "C" void func_80099E38(CpuContext*);
static void TagBufferSizePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagBufferSize(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099E38, TagBufferSizePort>("TagBufferSize", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099E38, MH3_TagBufferSize, "TagBufferSize");

extern "C" void func_8009A07C(CpuContext*);
static void TagCommandSizePort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_word(&ResourceMemory(), cpu->gpr[3], 4, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagCommandSize(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_8009A07C, TagCommandSizePort>("TagCommandSize", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x8009A07C, MH3_TagCommandSize, "TagCommandSize");

extern "C" void func_800994A0(CpuContext*);
static void ShpRelativeModelPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_offset(&ResourceMemory(), cpu->gpr[3], std::bit_cast<int32_t>(cpu->gpr[4]), 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ShpRelativeModel(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800994A0, ShpRelativeModelPort>("ShpRelativeModel", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800994A0, MH3_ShpRelativeModel, "ShpRelativeModel");

extern "C" void func_80099788(CpuContext*);
static void TagRelativeBytesPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_offset(&ResourceMemory(), cpu->gpr[3], std::bit_cast<int32_t>(cpu->gpr[4]), 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRelativeBytes(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099788, TagRelativeBytesPort>("TagRelativeBytes", cpu->gpr[1], cpu->gpr[3], cpu->gpr[4]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099788, MH3_TagRelativeBytes, "TagRelativeBytes");

extern "C" void func_80062918(CpuContext*);
static void ResDictionaryValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_ResDictionaryValid(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80062918, ResDictionaryValidPort>("ResDictionaryValid", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80062918, MH3_ResDictionaryValid, "ResDictionaryValid");

extern "C" void func_80077744(CpuContext*);
static void MatValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_MatValid(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80077744, MatValidPort>("MatValid", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80077744, MH3_MatValid, "MatValid");

extern "C" void func_800995F0(CpuContext*);
static void TagValidPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_valid(&ResourceMemory(), cpu->gpr[3], &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagValid(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800995F0, TagValidPort>("TagValid", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800995F0, MH3_TagValid, "TagValid");

extern "C" void func_800995DC(CpuContext*);
static void TagRefUncheckedConstPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefUncheckedConst(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_800995DC, TagRefUncheckedConstPort>("TagRefUncheckedConst", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x800995DC, MH3_TagRefUncheckedConst, "TagRefUncheckedConst");

extern "C" void func_80099808(CpuContext*);
static void TagRefUncheckedPort(CpuContext* cpu) {
    uint32_t value = 0;
    const auto status = mh3_res_runtime_reference(&ResourceMemory(), cpu->gpr[3], 0, &value);
    cpu->gpr[3] = Result(status, value);
}
extern "C" void MH3_TagRefUnchecked(CpuContext* cpu) {
    cpu->gpr[3] = Accessor<func_80099808, TagRefUncheckedPort>("TagRefUnchecked", cpu->gpr[1], cpu->gpr[3]);
}
REGISTER_NATIVE_FUNCTION_AS(0x80099808, MH3_TagRefUnchecked, "TagRefUnchecked");
