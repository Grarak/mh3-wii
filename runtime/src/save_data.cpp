// Save data, RMHP08. C ports of the save file read request, the character
// load, the new character and its store (decompiled from the translation).
//
// A character keeps the four random streams' seeds (ran_suu.cpp) at +0x3920:
// UserDataStore writes the current ones there and LoadCharacterWork puts them
// back, so a character's stream 1 (its charms) stays on the cycle it had
// when it was created.
//
// The save manager (0x806D8710) reads one save file (data00..05, 0x14000
// bytes) at a time into the system's save buffer (system_w+0x950):
//   +0x00..+0x10 status words, +0x14 the file's NAND path, +0x54 the buffer,
//   +0x58 its size, +0x5C the request (5: read), +0x60 the result, +0x64,
//   +0x100 s8 file index, +0x101 s8 (-1 on a read).
// A save file: +0 u32 flags (non-zero: in use), +4 u32 sum of the bytes from
// +8, then from +0x48 three character records of 0x6000 bytes.
#include "save_data.h"

#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

namespace {

constexpr uint32_t kSystem = 0x8065AA60;
constexpr uint32_t kSave = 0x806D8710;
constexpr uint32_t kSaveNames = 0x80607798; // const char* [6]: the save files' NAND paths
enum : uint32_t {
    kSystemCharacterWords = 0x18, // 4 u16 copied from the character's +0x3920
    kSystemCharacterId = 0x890,   // u32 copied from the character's +0x1C
    kSystemSaveBuffer = 0x950,    // u32: the save file buffer
    kSystemCharacter = 0x95C,     // u32: the working character record
};

enum : uint32_t {
    kSaveName = 0x14,
    kSaveBuffer = 0x54,
    kSaveSize = 0x58,
    kSaveRequest = 0x5C,
    kSaveResult = 0x60,
    kSaveProgress = 0x64,
    kSaveFile = 0x100,
    kSaveAccess = 0x101,
};

constexpr uint32_t kRequestRead = 5;
enum : uint32_t {
    kCharacterWords = 0x3920,
    kCharacterId = 0x1C
};

constexpr int32_t kWorkingCharacterSda = -17824; // r13-relative copy of system_w+0x95C
enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    OSGetTime = 0x804D53D0,
    Strcpy = 0x8045FBD4
};

// The character's random seeds: 32 u16 at +0x3920, the first four the
// streams' (system_w+0x18) when the character was stored.
constexpr uint32_t kCharacterSeeds = 32;
constexpr uint32_t kStreams = 4;

// 0x80049F6C LoadCharacterWork(record): copies a character record into the
// working character and takes a few of its fields into the system work.
void LoadCharacterWork(CpuContext* ctx, uint32_t record) {
    const uint32_t work = Memory::Read32(kSystem + kSystemCharacter);
    Memory::Write32(ctx->gpr[13] + uint32_t(kWorkingCharacterSda), work);
    {
        GuestFrame frame{ctx, 16};
        GuestCall{ctx}(Memcpy, {work, record, SaveData::kCharacterSize});
    }

    for (uint32_t i = 0; i < 4; ++i)
        Memory::Write16(kSystem + kSystemCharacterWords + 2 * i, Memory::Read16(work + kCharacterWords + 2 * i));

    Memory::Write32(kSystem + kSystemCharacterId, Memory::Read32(work + kCharacterId));
}

// 0x80049818 NewUserData(): clears the working character and seeds it, and
// the four random streams, from the low 16 bits of the time base. Only the
// boot (0x80046CE4) calls it, and the boot task's first step (0x80059648)
// sets the streams to 0x10 after it, so a new character stores those.
void NewUserData(CpuContext* ctx) {
    const uint32_t work = Memory::Read32(kSystem + kSystemCharacter);
    Memory::Write32(ctx->gpr[13] + uint32_t(kWorkingCharacterSda), work);
    uint16_t seed = 0;
    {
        GuestFrame frame{ctx, 16};
        GuestCall game{ctx};
        game.Call<Memset>({work, 0, SaveData::kCharacterSize});
        game.Call<OSGetTime>();
        seed = uint16_t(ctx->gpr[4]);
    }

    for (uint32_t i = 0; i < kCharacterSeeds; ++i)
        Memory::Write16(work + kCharacterWords + 2 * i, seed);

    for (uint32_t i = 0; i < kStreams; ++i)
        Memory::Write16(kSystem + kSystemCharacterWords + 2 * i, Memory::Read16(work + kCharacterWords + 2 * i));
}

// 0x80049F00 UserDataStore(dest): the streams' seeds and system_w+0x890 back
// into the working character, then the character to `dest`.
void UserDataStore(CpuContext* ctx, uint32_t dest) {
    const uint32_t work = Memory::Read32(kSystem + kSystemCharacter);
    Memory::Write32(ctx->gpr[13] + uint32_t(kWorkingCharacterSda), work);

    for (uint32_t i = 0; i < kStreams; ++i)
        Memory::Write16(work + kCharacterWords + 2 * i, Memory::Read16(kSystem + kSystemCharacterWords + 2 * i));

    Memory::Write32(work + kCharacterId, Memory::Read32(kSystem + kSystemCharacterId));
    GuestFrame frame{ctx, 16};
    GuestCall{ctx}(Memcpy, {dest, work, SaveData::kCharacterSize});
}

} // namespace

// 0x80049818 NewUserData().
extern "C" void func_80049818(CpuContext* ctx);
static void NewUserDataPort(CpuContext* ctx) {
    NewUserData(ctx);
}

extern "C" void MH3_NewUserData(CpuContext* ctx) {
    PortCheck::Call(ctx, "NewUserData", func_80049818, NewUserDataPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049818, MH3_NewUserData, "NewUserData");

// 0x80049F00 UserDataStore(dest).
extern "C" void func_80049F00(CpuContext* ctx);
static void UserDataStorePort(CpuContext* ctx) {
    UserDataStore(ctx, ctx->gpr[3]);
}

extern "C" void MH3_UserDataStore(CpuContext* ctx) {
    PortCheck::Call(ctx, "UserDataStore", func_80049F00, UserDataStorePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049F00, MH3_UserDataStore, "UserDataStore");

// 0x80441AD8 SaveReadRequest(s8 file, s8 access) -> 1, or 0 without a buffer:
// prepares the read of save file `file` into the save buffer (cleared).
extern "C" void func_80441AD8(CpuContext* ctx);
static void SaveReadRequestPort(CpuContext* ctx) {
    const auto file = int8_t(ctx->gpr[3]), access = int8_t(ctx->gpr[4]);

    for (uint32_t word = 0; word <= 0x10; word += 4)
        Memory::Write32(kSave + word, 0);

    Memory::Write8(kSave + kSaveFile, uint8_t(file));
    Memory::Write8(kSave + kSaveAccess, uint8_t(access));
    uint32_t result = 0;
    {
        GuestFrame frame{ctx, 16};
        GuestCall game{ctx};
        game.Call<Strcpy>({kSave + kSaveName, Memory::Read32(kSaveNames + 4 * uint32_t(int32_t(file)))});
        const uint32_t buffer = Memory::Read32(kSystem + kSystemSaveBuffer);
        Memory::Write32(kSave + kSaveBuffer, buffer);

        if (buffer) {
            Memory::Write32(kSave + kSaveSize, SaveData::kFileSize);
            Memory::Write32(kSave + kSaveRequest, kRequestRead);
            game.Call<Memset>({buffer, 0, SaveData::kFileSize});
            Memory::Write32(kSave + kSaveProgress, 0);
            Memory::Write32(kSave + kSaveResult, 0);
            result = 1;
        }
    }

    ctx->gpr[3] = result;
}

extern "C" void MH3_SaveReadRequest(CpuContext* ctx) {
    PortCheck::Call(ctx, "SaveReadRequest", func_80441AD8, SaveReadRequestPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80441AD8, MH3_SaveReadRequest, "SaveReadRequest");

// 0x80049F6C LoadCharacterWork(record).
extern "C" void func_80049F6C(CpuContext* ctx);
static void LoadCharacterWorkPort(CpuContext* ctx) {
    LoadCharacterWork(ctx, ctx->gpr[3]);
}

extern "C" void MH3_LoadCharacterWork(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadCharacterWork", func_80049F6C, LoadCharacterWorkPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80049F6C, MH3_LoadCharacterWork, "LoadCharacterWork");

// 0x80442088 LoadCharacter(u8 slot) -> 1, or 0 when the save buffer's sum is
// wrong or the file is unused: makes character `slot` of the save buffer the
// working character.
uint32_t SaveData::LoadCharacter(CpuContext* ctx, uint8_t slot) {
    const uint32_t file = Memory::Read32(kSave + kSaveBuffer);
    const uint8_t* bytes = Memory::GetPointer(file, kFileSize);
    uint32_t sum = 0;

    for (uint32_t i = 8; i < kFileSize; ++i)
        sum += bytes[i];

    if (Memory::Read32(file + 4) != sum || Memory::Read32(file) == 0)
        return 0;

    LoadCharacterWork(ctx, file + kCharacters + slot * kCharacterSize);
    return 1;
}

extern "C" void func_80442088(CpuContext* ctx);
static void LoadCharacterPort(CpuContext* ctx) {
    ctx->gpr[3] = SaveData::LoadCharacter(ctx, uint8_t(ctx->gpr[3]));
}

extern "C" void MH3_LoadCharacter(CpuContext* ctx) {
    PortCheck::Call(ctx, "LoadCharacter", func_80442088, LoadCharacterPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80442088, MH3_LoadCharacter, "LoadCharacter");
