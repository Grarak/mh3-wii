// RMHP08 field resource ABI. The C core owns selection and ordered scheduling;
// this boundary borrows checked CPU metadata and calls existing resource jobs.
#include "abi_bridge.h"
#include "fatal.h"
#include "guest_call.h"
#include "runtime/native/mh3_field_resources.h"

#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t PrimaryDescriptor = 0x806BDC14, AlternateDescriptor = 0x806BDC18;
constexpr uint32_t EffectTables = 0x8058A0CC, ExtraTables = 0x805893B4;
constexpr size_t TableViewBytes = 256 * 8, IdBytes = 0x3C - 8;

struct FieldResources {
    GuestCall call;
    uint32_t idsAddress;
    uint8_t areaCount = 0;
};

uint32_t Descriptor(GuestCall &call, uint8_t map) {
    const auto kind = uint8_t(call(0x802B06CC, {map}));
    return mh3_field_resources_descriptor(map, kind, Memory::Read32(PrimaryDescriptor), Memory::Read32(AlternateDescriptor));
}

mh3_field_res_status Table(void *user, unsigned kind, uint8_t map, uint8_t area, mh3_field_res_table *out) {
    auto &state = *static_cast<FieldResources *>(user);
    uint32_t record = 0;
    size_t bytes = TableViewBytes;

    if (kind == MH3_FIELD_RES_PRIMARY) {
        const uint32_t descriptor = Descriptor(state.call, map);

        if (!Memory::Contains(descriptor, 8))
            return MH3_FIELD_RES_RANGE;

        state.areaCount = Memory::Read8(descriptor);

        // Check before borrowing any array entry: invalid development area
        // overrides can otherwise turn adjacent position floats into pointers.
        if (area >= state.areaCount)
            return MH3_FIELD_RES_RANGE;

        const uint32_t pointers = Memory::Read32(descriptor + 4);
        const size_t pointerBytes = size_t(state.areaCount) * 4;

        if (!Memory::Contains(pointers, pointerBytes))
            return MH3_FIELD_RES_RANGE;

        const auto status = mh3_field_resources_area({Memory::GetPointer(descriptor, 8), 8}, {Memory::GetPointer(pointers, pointerBytes), pointerBytes}, area, &record);

        if (status != MH3_FIELD_RES_OK)
            return status;

        bytes = 16;
    } else {
        const uint32_t root = Memory::Read32((kind == MH3_FIELD_RES_EFFECT ? EffectTables : ExtraTables) + uint32_t(map) * 4);

        if (root) {
            if (!Memory::Contains(root + uint32_t(area) * 4, 4))
                return MH3_FIELD_RES_RANGE;

            record = Memory::Read32(root + uint32_t(area) * 4);
        }

        if (!record) {
            *out = {};
            return MH3_FIELD_RES_OK;
        }
    }

    if (!Memory::Contains(record, bytes))
        return MH3_FIELD_RES_RANGE;

    *out = {{Memory::GetPointer(record, bytes), bytes}, record};
    return MH3_FIELD_RES_OK;
}

void Primary(void *user, const mh3_field_res_pair *pair, uint8_t mode, uint8_t map, uint8_t area, uint8_t part) {
    static_cast<FieldResources *>(user)->call(0x802AE4E0, {pair->path, pair->bytes, mode, map, area, part});
}

uint8_t OnlineMode(void *user) {
    return uint8_t(static_cast<FieldResources *>(user)->call(0x800CF26C));
}

void OnlinePrepare(void *user, uint8_t map, uint8_t area) {
    auto &call = static_cast<FieldResources *>(user)->call;
    call(0x800CF688);
    call(0x802970F0);
    call(0x802B2370, {map, area});
}

void EnvironmentPrepare(void *user, uint8_t map, uint8_t area) {
    static_cast<FieldResources *>(user)->call(0x80433660, {map, area});
}

uint8_t Request(void *user, uint8_t kind, uint8_t map, uint8_t area, uint8_t index) {
    return uint8_t(static_cast<FieldResources *>(user)->call(0x800F6BD0, {1, kind, map, area, index}));
}

void Release(void *user, uint8_t id) {
    static_cast<FieldResources *>(user)->call(0x800F77DC, {id});
}

void Publish(void *user, uint8_t kind, uint32_t table, const uint8_t *, size_t) {
    auto &state = *static_cast<FieldResources *>(user);
    state.call(0x800F6A04, {kind, table, state.idsAddress});
}
} // namespace

extern "C" void MH3_FieldResourceDescriptor(CpuContext *cpu) {
    uint32_t descriptor = 0;
    {
        GuestFrame frame(cpu, 0x10);
        GuestCall call{cpu};
        descriptor = Descriptor(call, uint8_t(frame.Arg(3)));
    }

    cpu->gpr[3] = descriptor;
}
REGISTER_NATIVE_FUNCTION_AS(0x802AE5C8, MH3_FieldResourceDescriptor, "FieldResourceDescriptor");

extern "C" void MH3_ScheduleFieldResources(CpuContext *cpu) {
    GuestFrame frame(cpu, 0x60);
    const auto mode = uint8_t(frame.Arg(3)), map = uint8_t(frame.Arg(4)), area = uint8_t(frame.Arg(5));
    FieldResources state{{cpu}, cpu->gpr[1] + 8};
    const mh3_field_res_callbacks callbacks{&state, Table, Primary, OnlineMode, OnlinePrepare, EnvironmentPrepare, Request, Release, Publish};
    const auto status = mh3_field_resources_schedule(mode, map, area, Memory::GetPointer(state.idsAddress, IdBytes), IdBytes, &callbacks);

    if (status != MH3_FIELD_RES_OK)
        MH3_THROW(std::runtime_error("Native field resource scheduling failed: map " + std::to_string(map) + ", area " + std::to_string(area) + ", descriptor areas " + std::to_string(state.areaCount) +
                                 ", status " + std::to_string(status)));
}
REGISTER_NATIVE_FUNCTION_AS(0x802AE624, MH3_ScheduleFieldResources, "ScheduleFieldResources");
