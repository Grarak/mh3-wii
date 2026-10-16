// The game's network buffers and packets (Capcom's net library, RMHP08
// 0x803C98E0..0x803CA060, 0x803F8514..0x803FAEF0; decompiled with Ghidra,
// the call arguments checked in the translated C++), ported natively:
// NetworkBuffer (data, capacity, length; serialize, duplicate, the XOR
// scramble and CRC16), NetworkUniqueId, the packet stream (a 3-byte header:
// payload length, flags with a timestamp bit and a count of extra words),
// the unit packet a connection sends (a 0x16-byte header: magic, length,
// sequence, ack, sender, receiver, flags, CRC and the scramble keys) and the
// unit packet pool that orders reliable packets by sequence. Byte order goes
// through the platform object's hooks (identities on the Wii, taken
// directly); its log calls, the objects' own virtuals and the guest's
// memcpy, memset, memmove, memcmp and rand stay guest calls.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"

#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

#include <bit>
#include <cmath>
#include <initializer_list>

namespace {

enum : uint32_t {
    Memcpy = 0x80004000,
    Memset = 0x80004350,
    OperatorDelete = 0x80040564,
    Memmove = 0x8045BC18,
    Memcmp = 0x8045BD3C,
    Rand = 0x8045E620,
    OSReport = 0x804CDCA0,
};

constexpr uint32_t kPlatformPtr = 0x80797180;
constexpr uint32_t kUnitMagic = 0x80795E18;  // u16
constexpr uint32_t kCrcTable = 0x805713E0;   // 256 u16s
constexpr uint32_t kTimeDefault = 0x8079EC80; // f32
constexpr uint32_t kTimeScale = 0x8079EC84;  // f32: timestamp units per second

// Vtables.
enum : uint32_t {
    kBufferVtable = 0x805F9910,
    kUniqueIdVtable = 0x805FD418,
    kPoolVtable = 0x805FD558,
    kUnitVtable = 0x805FD594,
    kReaderVtable = 0x805FD5D0,
};

// The platform object's hooks.
enum : uint32_t {
    vInfo = 0xC,
    vError = 0x10,
    vWarn = 0x14,
    vNtohs = 0x48,
    vHtons = 0x4C,
    vNtohl = 0x50,
    vHtonl = 0x54,
};

// A NetworkBuffer: vtable, data, capacity, length; a stream or unit packet
// adds its current packet, a reader its read cursor, a pool its next
// sequence (u16) and cursor.
enum : uint32_t {
    bData = 0x4,
    bCapacity = 0x8,
    bLength = 0xC,
    bPacket = 0x10,
    bCursor = 0x14,
    bSequence = 0x10,
};

// Virtual slots the code calls on its objects.
enum : uint32_t {
    vReset = 0x10,
    vAttach = 0x14,
    vLoad = 0x1C,
    vExport = 0x20,
    vImport = 0x24,
    vCrc = 0x38,
};

// Strings for the log hooks.
enum : uint32_t {
    kMsgInitNoBuffer = 0x805F9570,
    kMsgSerializeNoBuffer = 0x805F959C,
    kMsgSerializeNoData = 0x805F95CC,
    kMsgSerializeTooSmall = 0x805F9600,
    kMsgSerializeEmpty = 0x805F963C,
    kMsgDeserializeNoBuffer = 0x805F9670,
    kMsgDeserializeNoData = 0x805F96A0,
    kMsgDeserializeTooBig = 0x805F96D8,
    kMsgDeserializeEmpty = 0x805F9710,
    kMsgDuplicateNoBuffer = 0x805F9748,
    kMsgDuplicateNoObject = 0x805F9778,
    kMsgDuplicateNoData = 0x805F97A8,
    kMsgDuplicateTooBig = 0x805F97DC,
    kMsgDuplicateEmpty = 0x805F9814,
    kMsgEqualsNoBuffer = 0x805F9848,
    kMsgEqualsNoObject = 0x805F9874,
    kMsgEqualsNoData = 0x805F98A0,
    kMsgEqualsEmpty = 0x805F98D0,
    kMsgIdValidNull = 0x805FD190,
    kMsgIdImportNull = 0x805FD1B8,
    kMsgIdImportNoData = 0x805FD1E4,
    kMsgIdImportTooLong = 0x805FD214,
    kMsgIdImportEmpty = 0x805FD248,
    kMsgIdExportNull = 0x805FD278,
    kMsgIdExportNoData = 0x805FD2A0,
    kMsgIdExportTooLong = 0x805FD2D0,
    kMsgIdEqualsFirst = 0x805FD300,
    kMsgIdEqualsSecond = 0x805FD328,
    kMsgIdObjectExport = 0x805FD350,
    kMsgIdObjectEqualsThis = 0x805FD388,
    kMsgIdObjectEqualsArg = 0x805FD3BC,
    kMsgIdObjectEqualsData = 0x805FD3E4,
    kMsgUnitCrc = 0x805FD458,
    kMsgUnitMagic = 0x805FD488,
    kMsgPoolPutTop = 0x805FD4BC,
    kMsgPoolPutLow = 0x805FD4F0,
    kMsgPoolPutAll = 0x805FD524,
};

constexpr uint32_t kPoolFull = 0x80030032;
constexpr uint32_t kUnitHeader = 0x16;
constexpr uint32_t kCrFloatArgs = 1u << (31 - 6);

uint32_t Read(uint32_t address) {
    return Memory::Read32(address);
}

int32_t ReadInt(uint32_t address) {
    return int32_t(Memory::Read32(address));
}

void Write(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

uint8_t Byte(uint32_t address) {
    return Memory::Read8(address);
}

float ReadFloat(uint32_t address) {
    return std::bit_cast<float>(Memory::Read32(address));
}

// Unaligned big-endian halfword and word (the original's 2- and 4-byte
// memcpy through a local).
uint32_t Load16(uint32_t at) {
    return uint32_t(Byte(at)) << 8 | Byte(at + 1);
}

uint32_t Load32(uint32_t at) {
    return Load16(at) << 16 | Load16(at + 2);
}

void Store16(uint32_t at, uint32_t value) {
    Memory::Write8(at, uint8_t(value >> 8));
    Memory::Write8(at + 1, uint8_t(value));
}

void Store32(uint32_t at, uint32_t value) {
    Store16(at, value >> 16);
    Store16(at + 2, value);
}

void Copy(CpuContext* cpu, uint32_t to, uint32_t from, uint32_t length) {
    GuestCall{cpu}.Call<Memcpy>({to, from, length});
}

uint32_t Platform() {
    return Read(kPlatformPtr);
}

// The platform's byte order hooks: identities on the Wii, taken directly.
bool IsIdentity(uint32_t fn) {
    return fn == 0x80418430 || fn == 0x80418438 || fn == 0x80418440 || fn == 0x80418448;
}

uint32_t Convert(CpuContext* cpu, uint32_t slot, uint32_t value) {
    const uint32_t platform = Platform();
    const uint32_t fn = Read(Read(platform) + slot);
    return IsIdentity(fn) ? value : GuestCall{cpu}(fn, {platform, value});
}

// The platform's log hooks (variadic).
void Log(CpuContext* cpu, uint32_t slot, uint32_t message, uint32_t a = 0, uint32_t b = 0) {
    cpu->cr &= ~kCrFloatArgs;
    GuestCall{cpu}.Virtual(Platform(), slot, {message, a, b});
}

uint32_t Remaining(uint32_t object, uint32_t at) {
    return Read(object + bLength) - (at - Read(object + bData));
}

// PPC fctiwz: toward zero, saturating, NaN as INT_MIN.
int32_t Fctiwz(double value) {
    if (std::isnan(value) || value <= -2147483648.0)
        return INT32_MIN;

    if (value >= 2147483647.0)
        return INT32_MAX;

    return int32_t(value);
}

// 0x803C98E0: the base NetworkBuffer, empty.
void BufferInit(uint32_t buffer) {
    Write(buffer, kBufferVtable);
    Write(buffer + bData, 0);
    Write(buffer + bCapacity, 0);
    Write(buffer + bLength, 0);
}

// 0x803C9900 and the derived destructors: deletes a heap object.
uint32_t BufferDestroy(CpuContext* cpu, uint32_t buffer, uint32_t flags) {
    if (buffer && int16_t(flags) > 0)
        GuestCall{cpu}.Call<OperatorDelete>({buffer});

    return buffer;
}

// 0x803C9960: a buffer over `data`, cleared.
void BufferAssign(CpuContext* cpu, uint32_t buffer, uint32_t data, uint32_t capacity) {
    Write(buffer + bData, data);
    Write(buffer + bCapacity, capacity);
    Write(buffer + bLength, 0);
    GuestCall{cpu}.Call<Memset>({data, 0, capacity});
}

// 0x803C997C.
void BufferClear(CpuContext* cpu, uint32_t buffer) {
    if (!Read(buffer + bData))
        Log(cpu, vError, kMsgInitNoBuffer);
    else
        GuestCall{cpu}.Call<Memset>({Read(buffer + bData), 0, Read(buffer + bCapacity)});
}

// 0x803C99E0: a buffer over `length` bytes of data.
void BufferAttach(uint32_t buffer, uint32_t data, uint32_t length) {
    Write(buffer + bData, data);
    Write(buffer + bCapacity, length);
    Write(buffer + bLength, length);
}

// 0x803C99F0: copies the contents out.
int32_t BufferSerialize(CpuContext* cpu, uint32_t buffer, uint32_t to, uint32_t most) {
    if (!Read(buffer + bData)) {
        Log(cpu, vError, kMsgSerializeNoBuffer);
        return -1;
    }

    if (!to) {
        Log(cpu, vError, kMsgSerializeNoData);
        return -1;
    }

    const uint32_t length = Read(buffer + bLength);

    if (most < length) {
        Log(cpu, vError, kMsgSerializeTooSmall);
        return -1;
    }

    if (!length) {
        Log(cpu, vWarn, kMsgSerializeEmpty);
        return 0;
    }

    Copy(cpu, to, Read(buffer + bData), length);
    return ReadInt(buffer + bLength);
}

// 0x803C9AF4: copies contents in.
int32_t BufferDeserialize(CpuContext* cpu, uint32_t buffer, uint32_t from, uint32_t length) {
    if (!Read(buffer + bData)) {
        Log(cpu, vError, kMsgDeserializeNoBuffer);
        return -1;
    }

    if (!from) {
        Log(cpu, vError, kMsgDeserializeNoData);
        return -1;
    }

    if (Read(buffer + bCapacity) < length) {
        Log(cpu, vError, kMsgDeserializeTooBig);
        return -1;
    }

    if (!length) {
        Log(cpu, vWarn, kMsgDeserializeEmpty);
        return 0;
    }

    Write(buffer + bLength, length);
    Copy(cpu, Read(buffer + bData), from, length);
    return ReadInt(buffer + bLength);
}

// 0x803C9BF4: copies another buffer's contents.
int32_t BufferDuplicate(CpuContext* cpu, uint32_t buffer, uint32_t other) {
    if (!Read(buffer + bData)) {
        Log(cpu, vError, kMsgDuplicateNoBuffer);
        return -1;
    }

    if (!other) {
        Log(cpu, vError, kMsgDuplicateNoObject);
        return -1;
    }

    if (!Read(other + bData)) {
        Log(cpu, vError, kMsgDuplicateNoData);
        return -1;
    }

    const uint32_t length = Read(other + bLength);

    if (Read(buffer + bCapacity) < length) {
        Log(cpu, vError, kMsgDuplicateTooBig);
        return -1;
    }

    if (!length) {
        Log(cpu, vWarn, kMsgDuplicateEmpty);
        return 0;
    }

    Copy(cpu, Read(buffer + bData), Read(other + bData), length);
    Write(buffer + bLength, Read(other + bLength));
    return ReadInt(other + bLength);
}

// 0x803C9D3C: the same contents.
bool BufferEquals(CpuContext* cpu, uint32_t buffer, uint32_t other) {
    if (!Read(buffer + bData)) {
        Log(cpu, vError, kMsgEqualsNoBuffer);
        return false;
    }

    if (!other) {
        Log(cpu, vError, kMsgEqualsNoObject);
        return false;
    }

    if (!Read(other + bData)) {
        Log(cpu, vError, kMsgEqualsNoData);
        return false;
    }

    if (Read(other + bLength) != Read(buffer + bLength))
        return false;

    if (!Read(buffer + bLength)) {
        Log(cpu, vWarn, kMsgEqualsEmpty);
        return true;
    }

    return GuestCall{cpu}.Call<Memcmp>({Read(other + bData), Read(buffer + bData), Read(buffer + bLength)}) == 0;
}

// 0x803C9E4C / 0x803C9F58: the XOR scramble from `offset`, each byte keyed
// by the plain byte before it.
void Scramble(uint32_t buffer, uint32_t key, uint32_t offset, uint32_t length) {
    uint32_t at = Read(buffer + bData) + (offset & 0xFFFF);
    uint8_t previous = uint8_t(key);

    for (uint32_t i = 0; i < (length & 0xFFFF); ++i, ++at) {
        const uint8_t plain = Byte(at);
        Memory::Write8(at, plain ^ previous);
        previous = plain;
    }
}

void Unscramble(uint32_t buffer, uint32_t key, uint32_t offset, uint32_t length) {
    uint32_t at = Read(buffer + bData) + (offset & 0xFFFF);
    uint8_t previous = uint8_t(key);

    for (uint32_t i = 0; i < (length & 0xFFFF); ++i, ++at) {
        previous = Byte(at) ^ previous;
        Memory::Write8(at, previous);
    }
}

// 0x803CA060: CRC16 (table at 0x805713E0, from 0xFFFF) of the first bytes.
uint32_t Crc16(uint32_t buffer, uint32_t length) {
    static uint16_t table[256];
    static bool loaded;

    if (!loaded) {
        for (uint32_t i = 0; i < 256; ++i)
            table[i] = Memory::Read16(kCrcTable + 2 * i);

        loaded = true;
    }

    uint32_t crc = 0xFFFF, at = Read(buffer + bData);

    for (uint32_t i = 0; i < (length & 0xFFFF); ++i)
        crc = ((crc << 8) ^ table[(crc >> 8) ^ Byte(at + i)]) & 0xFFFF;

    return crc;
}

// 0x803CBA18 / 0x803CBA54: a packet reader / unit packet, empty.
void ReaderConstruct(uint32_t reader) {
    BufferInit(reader);
    Write(reader, kReaderVtable);
}

void UnitConstruct(uint32_t unit) {
    BufferInit(unit);
    Write(unit, kUnitVtable);
}

// 0x803F851C: a unique ID's type is 1..5.
bool IdIsValid(CpuContext* cpu, uint32_t id) {
    if (!id) {
        Log(cpu, vError, kMsgIdValidNull);
        return false;
    }

    const uint8_t type = Byte(id);
    return type != 0 && type <= 5;
}

// 0x803F8588: a unique ID (0xE bytes: type, 3 zero bytes, up to 10 bytes).
void IdImport(CpuContext* cpu, uint32_t id, uint32_t type, uint32_t data, uint32_t length) {
    if (!id) {
        Log(cpu, vError, kMsgIdImportNull);
    } else if (!data) {
        Log(cpu, vError, kMsgIdImportNoData);
    } else if (length >= 0xB) {
        Log(cpu, vError, kMsgIdImportTooLong);
    } else if (!length) {
        Log(cpu, vWarn, kMsgIdImportEmpty);
    } else {
        GuestCall{cpu}.Call<Memset>({id, 0, 0xE});
        Memory::Write8(id, uint8_t(type));
        Memory::Write8(id + 1, 0);
        Memory::Write8(id + 2, 0);
        Memory::Write8(id + 3, 0);
        Copy(cpu, id + 4, data, length);
    }
}

// 0x803F86B4.
void IdExport(CpuContext* cpu, uint32_t id, uint32_t to, uint32_t length) {
    if (!id)
        Log(cpu, vError, kMsgIdExportNull);
    else if (!to)
        Log(cpu, vError, kMsgIdExportNoData);
    else if (length < 0xB)
        Copy(cpu, to, id + 4, length);
    else
        Log(cpu, vError, kMsgIdExportTooLong);
}

// 0x803F8764: the same ID (types 1..4 compare; 5 never equals).
bool IdEquals(CpuContext* cpu, uint32_t a, uint32_t b) {
    if (!a) {
        Log(cpu, vError, kMsgIdEqualsFirst);
        return false;
    }

    if (!b) {
        Log(cpu, vError, kMsgIdEqualsSecond);
        return false;
    }

    if (Byte(a) != Byte(b) || uint32_t(Byte(a)) - 1 >= 4)
        return false;

    return GuestCall{cpu}.Call<Memcmp>({a, b, 0xE}) == 0;
}

// 0x803F881C: a NetworkUniqueId object over its own 0xE bytes.
void IdObjectConstruct(CpuContext* cpu, uint32_t object) {
    BufferInit(object);
    Write(object, kUniqueIdVtable);
    GuestCall{cpu}.Virtual(object, vAttach, {object + 0x10, 0xE});
}

// 0x803F8874.
bool IdObjectIsValid(CpuContext* cpu, uint32_t object) {
    return Read(object + bData) && IdIsValid(cpu, object + 0x10);
}

// 0x803F8894.
void IdObjectImport(CpuContext* cpu, uint32_t object, uint32_t type, uint32_t data, uint32_t length) {
    GuestCall{cpu}.Virtual(object, vAttach, {object + 0x10, 0xE});
    Write(object + bLength, Read(object + bCapacity));
    IdImport(cpu, object + 0x10, type, data, length);
}

// 0x803F8914.
void IdObjectExport(CpuContext* cpu, uint32_t object, uint32_t to, uint32_t length) {
    if (!Read(object + bData))
        Log(cpu, vError, kMsgIdObjectExport);
    else
        IdExport(cpu, object + 0x10, to, length);
}

// 0x803F8968.
bool IdObjectEquals(CpuContext* cpu, uint32_t object, uint32_t other) {
    if (!Read(object + bData)) {
        Log(cpu, vError, kMsgIdObjectEqualsThis);
        return false;
    }

    if (!other) {
        Log(cpu, vError, kMsgIdObjectEqualsArg);
        return false;
    }

    if (!Read(other + bData)) {
        Log(cpu, vError, kMsgIdObjectEqualsData);
        return false;
    }

    return IdEquals(cpu, object + 0x10, other + 0x10);
}

// The current stream packet's header fields.
uint32_t Flags(uint32_t stream) {
    return Byte(Read(stream + bPacket) + 2);
}

uint32_t ExtraCount(uint32_t stream) {
    return Flags(stream) & 0x3F;
}

bool HasTimestamp(uint32_t stream) {
    return Flags(stream) & 0x40;
}

// 0x803F9774: the header's length: 3, a timestamp, the extra words and
// their leading word.
uint32_t HeaderSize(uint32_t stream) {
    const uint32_t count = ExtraCount(stream);
    return (HasTimestamp(stream) ? 4 : 0) + (count ? 4 : 0) + count * 4 + 3;
}

// 0x803F972C: the payload's length.
uint32_t PayloadLength(CpuContext* cpu, uint32_t stream) {
    return Convert(cpu, vNtohs, Load16(Read(stream + bPacket))) & 0xFFFF;
}

// 0x803F966C: the packet's length with the header parts `mask` keeps (0x40
// the timestamp, 0x3F the extra words).
uint32_t PacketSize(CpuContext* cpu, uint32_t stream, uint32_t mask) {
    uint32_t size = (PayloadLength(cpu, stream) + 3) & 0xFFFF;

    if (mask & 0x40)
        size += HasTimestamp(stream) ? 4 : 0;

    if (mask & 0x3F) {
        const uint32_t count = ExtraCount(stream);
        size += (count ? 4 : 0) + count * 4;
    }

    return size;
}

// 0x803F9010: a whole packet is left.
bool HasPacket(CpuContext* cpu, uint32_t stream) {
    const uint32_t left = Remaining(stream, Read(stream + bPacket));

    if (left < (HeaderSize(stream) & 0xFFFF))
        return false;

    return left >= (PacketSize(cpu, stream, 0xFF) & 0xFFFF);
}

// 0x803F9350: the current packet's payload (0 without a whole packet).
uint32_t Payload(CpuContext* cpu, uint32_t stream) {
    const uint32_t left = Remaining(stream, Read(stream + bPacket));

    if (left < (HeaderSize(stream) & 0xFFFF) || left < (PacketSize(cpu, stream, 0xFF) & 0xFFFF))
        return 0;

    return Read(stream + bPacket) + (HeaderSize(stream) & 0xFFFF);
}

// 0x803F8FD0: the read cursor at the current packet's payload.
void ReaderRewind(CpuContext* cpu, uint32_t reader) {
    if (HasPacket(cpu, reader))
        Write(reader + bCursor, Payload(cpu, reader));
}

// 0x803F8F84: a reader over `length` bytes of packets.
void ReaderInit(CpuContext* cpu, uint32_t reader, uint32_t data, uint32_t length) {
    BufferAttach(reader, data, length);
    Write(reader + bPacket, data);
    Write(reader + bCursor, 0);
    ReaderRewind(cpu, reader);
}

// 0x803F909C: the next packet.
void ReaderNext(CpuContext* cpu, uint32_t reader) {
    const uint32_t left = Remaining(reader, Read(reader + bPacket));

    if ((HeaderSize(reader) & 0xFFFF) > left || (PacketSize(cpu, reader, 0xFF) & 0xFFFF) > left)
        return;

    Write(reader + bPacket, Read(reader + bPacket) + (PacketSize(cpu, reader, 0xFF) & 0xFFFF));
    Write(reader + bCursor, Payload(cpu, reader));
}

// 0x803F913C: copies the current packet with the header parts `mask`
// keeps; 0 without a whole one, -1 when it does not fit.
int32_t CopyPacket(CpuContext* cpu, uint32_t reader, uint32_t to, uint32_t most, uint32_t mask) {
    const uint32_t packet = Read(reader + bPacket);

    if (Remaining(reader, packet) < 3)
        return 0;

    const uint32_t size = PacketSize(cpu, reader, mask) & 0xFFFF;

    if (Remaining(reader, packet) < size)
        return 0;

    if (most < size)
        return -1;

    Copy(cpu, to, packet, 3);
    uint32_t at = 3;

    if (!(mask & 0x80))
        Memory::Write8(to + 2, Byte(to + 2) & 0x7F);

    if (!(mask & 0x40)) {
        Memory::Write8(to + 2, Byte(to + 2) & 0xBF);
    } else if (HasTimestamp(reader)) {
        Copy(cpu, to + 3, packet + 3, 4);
        at = 7;
    }

    if (!(mask & 0x3F)) {
        Memory::Write8(to + 2, Byte(to + 2) & 0xC0);
    } else if (const uint32_t count = ExtraCount(reader)) {
        const uint32_t skip = HasTimestamp(reader) ? 4 : 0;
        Copy(cpu, to + at, packet + skip + 3, 4);
        Copy(cpu, to + at + 4, packet + skip + 7, count * 4);
        at += 4 + count * 4;
    }

    const uint32_t payload = Payload(cpu, reader);

    if (!payload)
        return 0;

    Copy(cpu, to + at, payload, PayloadLength(cpu, reader));
    return int32_t((at + PayloadLength(cpu, reader)) & 0xFFFF);
}

// 0x803F9834: the packet's timestamp in seconds (the default without one).
float Timestamp(CpuContext* cpu, uint32_t stream) {
    if (!HasTimestamp(stream))
        return ReadFloat(kTimeDefault);

    const int32_t units = int32_t(Convert(cpu, vNtohl, Load32(Read(stream + bPacket) + 3)));
    return float(float(double(units)) / ReadFloat(kTimeScale));
}

// 0x803F8EEC: sets the packet's timestamp (when it has one).
void SetTimestamp(CpuContext* cpu, uint32_t stream, double seconds) {
    if (!HasTimestamp(stream))
        return;

    const int32_t units = Fctiwz(float(double(ReadFloat(kTimeScale)) * seconds));
    Store32(Read(stream + bPacket) + 3, Convert(cpu, vHtonl, uint32_t(units)));
}

// 0x803F8E8C: sets the payload length.
void SetPayloadLength(CpuContext* cpu, uint32_t stream, uint32_t length) {
    Store16(Read(stream + bPacket), Convert(cpu, vHtons, length & 0xFFFF));
}

// 0x803F8A8C: starts a packet at the writer's current one: flags 0x80 and
// 0x40 (a timestamp), `count` extra words after `first`; answers the
// header's length (-1 without room).
int32_t WriterBegin(CpuContext* cpu, uint32_t writer, uint32_t flag80, uint32_t timestamp, uint32_t first, uint32_t count, uint32_t words) {
    if (Read(writer + bCapacity) - Read(writer + bLength) < 3)
        return -1;

    SetPayloadLength(cpu, writer, 0);
    const uint32_t packet = Read(writer + bPacket);
    Memory::Write8(packet + 2, uint8_t((count & 0x3F) | (flag80 ? 0x80 : 0) | (timestamp ? 0x40 : 0)));

    if (Read(writer + bCapacity) - Read(writer + bLength) < (HeaderSize(writer) & 0xFFFF))
        return -1;

    SetTimestamp(cpu, writer, ReadFloat(kTimeDefault));

    if (count & 0xFF) {
        const uint32_t skip = timestamp ? 4 : 0;
        Store32(packet + skip + 3, Convert(cpu, vHtonl, first));

        for (uint32_t i = 0; i < (count & 0x3F); ++i)
            Store32(packet + skip + 7 + 4 * i, Convert(cpu, vHtonl, Read(words + 4 * i)));
    }

    Write(writer + bLength, Read(writer + bLength) + (HeaderSize(writer) & 0xFFFF));
    return int32_t(HeaderSize(writer) & 0xFFFF);
}

// 0x803F8C40: appends a unique ID (its 0xE bytes, through its exporter).
int32_t WriteId(CpuContext* cpu, uint32_t writer, uint32_t id) {
    if (Read(writer + bCapacity) - Read(writer + bLength) < 0xE)
        return -1;

    const int32_t written = int32_t(GuestCall{cpu}.Virtual(id, vExport, {Read(writer + bData) + Read(writer + bLength), 0xE}));

    if (written < 0)
        return -1;

    Write(writer + bLength, Read(writer + bLength) + uint32_t(written));
    return written;
}

// 0x803F8CC0: appends bytes.
int32_t WriteBytes(CpuContext* cpu, uint32_t writer, uint32_t from, uint32_t length) {
    if (Read(writer + bCapacity) - Read(writer + bLength) < length)
        return -1;

    Copy(cpu, Read(writer + bData) + Read(writer + bLength), from, length);
    Write(writer + bLength, Read(writer + bLength) + length);
    return int32_t(length);
}

// 0x803F8D2C / 0x803F8DC0 / 0x803F8E54: appends a word, halfword, byte.
int32_t WriteU32(CpuContext* cpu, uint32_t writer, uint32_t value) {
    if (Read(writer + bCapacity) - Read(writer + bLength) < 4)
        return -1;

    Store32(Read(writer + bData) + Read(writer + bLength), Convert(cpu, vHtonl, value));
    Write(writer + bLength, Read(writer + bLength) + 4);
    return 4;
}

int32_t WriteU16(CpuContext* cpu, uint32_t writer, uint32_t value) {
    if (Read(writer + bCapacity) - Read(writer + bLength) < 2)
        return -1;

    Store16(Read(writer + bData) + Read(writer + bLength), Convert(cpu, vHtons, value & 0xFFFF));
    Write(writer + bLength, Read(writer + bLength) + 2);
    return 2;
}

int32_t WriteU8(uint32_t writer, uint32_t value) {
    if (Read(writer + bCapacity) == Read(writer + bLength))
        return -1;

    Memory::Write8(Read(writer + bData) + Read(writer + bLength), uint8_t(value));
    Write(writer + bLength, Read(writer + bLength) + 1);
    return 1;
}

// 0x803F93EC: reads a unique ID (0xE bytes, through its importer).
uint32_t ReadId(CpuContext* cpu, uint32_t reader, uint32_t id) {
    const uint32_t cursor = Read(reader + bCursor);

    if (Remaining(reader, cursor) < 0xE)
        return 0;

    GuestFrame frame(cpu, 0x30);
    const uint32_t local = frame[0x10];
    Copy(cpu, local, cursor, 0xE);
    GuestCall{cpu}.Virtual(id, vImport, {local, 0xE});
    Write(reader + bCursor, Read(reader + bCursor) + 0xE);
    return 0xE;
}

// 0x803F947C: reads bytes.
uint32_t ReadBytes(CpuContext* cpu, uint32_t reader, uint32_t to, uint32_t length) {
    const uint32_t cursor = Read(reader + bCursor);

    if (Remaining(reader, cursor) < length)
        return 0;

    Copy(cpu, to, cursor, length);
    Write(reader + bCursor, Read(reader + bCursor) + length);
    return length & 0xFFFF;
}

// 0x803F94F0 / 0x803F9590 / 0x803F9630: reads a word, halfword, byte.
uint32_t ReadU32(CpuContext* cpu, uint32_t reader, uint32_t to) {
    const uint32_t cursor = Read(reader + bCursor);

    if (Remaining(reader, cursor) < 4)
        return 0;

    Store32(to, Convert(cpu, vNtohl, Load32(cursor)));
    Write(reader + bCursor, cursor + 4);
    return 4;
}

uint32_t ReadU16(CpuContext* cpu, uint32_t reader, uint32_t to) {
    const uint32_t cursor = Read(reader + bCursor);

    if (Remaining(reader, cursor) < 2)
        return 0;

    Store16(to, Convert(cpu, vNtohs, Load16(cursor)));
    Write(reader + bCursor, cursor + 2);
    return 2;
}

uint32_t ReadU8(uint32_t reader, uint32_t to) {
    const uint32_t cursor = Read(reader + bCursor);

    if (Read(reader + bLength) == cursor - Read(reader + bData))
        return 0;

    Memory::Write8(to, Byte(cursor));
    Write(reader + bCursor, cursor + 1);
    return 1;
}

// The unit packet's header fields (at its current packet).
uint32_t UnitField16(CpuContext* cpu, uint32_t unit, uint32_t offset) {
    return Convert(cpu, vNtohs, Load16(Read(unit + bPacket) + offset)) & 0xFFFF;
}

uint32_t UnitField32(CpuContext* cpu, uint32_t unit, uint32_t offset) {
    return Convert(cpu, vNtohl, Load32(Read(unit + bPacket) + offset));
}

void SetUnitField16(CpuContext* cpu, uint32_t unit, uint32_t offset, uint32_t value) {
    Store16(Read(unit + bPacket) + offset, Convert(cpu, vHtons, value & 0xFFFF));
}

void SetUnitField32(CpuContext* cpu, uint32_t unit, uint32_t offset, uint32_t value) {
    Store32(Read(unit + bPacket) + offset, Convert(cpu, vHtonl, value));
}

uint32_t UnitFlags(uint32_t unit) {
    return Byte(Read(unit + bPacket) + 0x11);
}

void SetUnitFlags(uint32_t unit, uint32_t flags) {
    Memory::Write8(Read(unit + bPacket) + 0x11, uint8_t(flags));
}

// 0x803FA174 / 0x803FA14C: the payload's length / the whole unit's.
uint32_t UnitPayloadLength(CpuContext* cpu, uint32_t unit) {
    return UnitField16(cpu, unit, 2);
}

uint32_t UnitLength(CpuContext* cpu, uint32_t unit) {
    return (UnitPayloadLength(cpu, unit) + kUnitHeader) & 0xFFFF;
}

// 0x803F9924: starts a unit packet (magic, version 4).
int32_t UnitBegin(CpuContext* cpu, uint32_t unit) {
    if (Read(unit + bCapacity) - Read(unit + bLength) < kUnitHeader)
        return -1;

    const uint32_t at = Read(unit + bData) + Read(unit + bLength);
    GuestCall{cpu}.Call<Memset>({at, 0, kUnitHeader});
    Store16(at, Convert(cpu, vHtons, Memory::Read16(kUnitMagic)));
    Memory::Write8(at + 0x10, 4);
    Write(unit + bLength, Read(unit + bLength) + kUnitHeader);
    return kUnitHeader;
}

// 0x803F99D4: appends the reader's current packet.
int32_t UnitAppend(CpuContext* cpu, uint32_t unit, uint32_t reader, uint32_t mask) {
    if (!HasPacket(cpu, reader))
        return -1;

    const int32_t copied = CopyPacket(cpu, reader, Read(unit + bData) + Read(unit + bLength), Read(unit + bCapacity) - Read(unit + bLength), mask);

    if (copied < 0)
        return -1;

    Write(unit + bLength, Read(unit + bLength) + uint32_t(copied));
    return copied;
}

// 0x803F9A74: reserves bytes.
int32_t UnitSkip(uint32_t unit, uint32_t length) {
    length &= 0xFFFF;

    if (Read(unit + bCapacity) - Read(unit + bLength) < length)
        return -1;

    Write(unit + bLength, Read(unit + bLength) + length);
    return int32_t(length);
}

void SetCrc(CpuContext* cpu, uint32_t unit, uint32_t crc) {
    SetUnitField16(cpu, unit, 0x12, crc);
}

void SetKeys(uint32_t unit, uint32_t a, uint32_t b) {
    Memory::Write8(Read(unit + bPacket) + 0x14, uint8_t(a));
    Memory::Write8(Read(unit + bPacket) + 0x15, uint8_t(b));
}

// 0x803F9D70: the CRC over the whole unit (the CRC and keys zero).
void UnitSeal(CpuContext* cpu, uint32_t unit) {
    SetCrc(cpu, unit, 0);
    SetKeys(unit, 0, 0);
    const uint32_t length = UnitLength(cpu, unit);
    SetCrc(cpu, unit, GuestCall{cpu}.Virtual(unit, vCrc, {length}));
}

// 0x803F9EA4: a whole unit is left.
bool UnitHasPacket(CpuContext* cpu, uint32_t unit) {
    const uint32_t left = Remaining(unit, Read(unit + bPacket));

    if (left < kUnitHeader)
        return false;

    return left >= UnitLength(cpu, unit);
}

// 0x803F9F20: copies the unit out.
int32_t UnitCopy(CpuContext* cpu, uint32_t unit, uint32_t to, uint32_t most) {
    const uint32_t left = Remaining(unit, Read(unit + bPacket));

    if (left < kUnitHeader)
        return 0;

    const uint32_t length = UnitLength(cpu, unit);

    if (left < length)
        return 0;

    if (most < length)
        return -1;

    Copy(cpu, to, Read(unit + bPacket), length);
    return int32_t(length);
}

// 0x803F9FD8: takes the unit off the front; answers its length.
uint32_t UnitConsume(CpuContext* cpu, uint32_t unit) {
    const uint32_t packet = Read(unit + bPacket);

    if (Remaining(unit, packet) < kUnitHeader)
        return 0;

    const uint32_t length = UnitLength(cpu, unit);

    if (Remaining(unit, packet) < length)
        return 0;

    const uint32_t left = Read(unit + bLength) - length;
    Write(unit + bLength, left);

    if (left)
        GuestCall{cpu}.Call<Memmove>({packet, packet + length, left});

    return length;
}

// 0x803FA07C: the unit's payload (0 without a whole unit).
uint32_t UnitPayload(CpuContext* cpu, uint32_t unit) {
    const uint32_t packet = Read(unit + bPacket);

    if (Remaining(unit, packet) < kUnitHeader || Remaining(unit, packet) < UnitLength(cpu, unit))
        return 0;

    return packet + kUnitHeader;
}

// 0x803FA3F4: scrambles the payload with a random key, hidden in the two
// key bytes by its low bit.
void UnitScramble(CpuContext* cpu, uint32_t unit) {
    GuestCall call{cpu};
    const uint8_t key = uint8_t(call.Call<Rand>());
    Scramble(unit, key, kUnitHeader, UnitPayloadLength(cpu, unit));

    if (!(key & 1)) {
        const uint8_t other = uint8_t(call.Call<Rand>());
        SetKeys(unit, uint8_t(~key), other);
    } else {
        const uint8_t other = uint8_t(call.Call<Rand>() & 0xFE);
        SetKeys(unit, other, uint8_t(~key));
    }
}

// 0x803FA49C.
void UnitUnscramble(CpuContext* cpu, uint32_t unit) {
    const uint32_t packet = Read(unit + bPacket);
    uint8_t key = uint8_t(~Byte(packet + 0x14));

    if (key & 1)
        key = uint8_t(~Byte(packet + 0x15));

    Unscramble(unit, key, kUnitHeader, UnitPayloadLength(cpu, unit));
}

// 0x803FA514: the unit's CRC and magic hold (else it is reset).
bool UnitVerify(CpuContext* cpu, uint32_t unit) {
    const uint32_t crc = UnitField16(cpu, unit, 0x12);
    SetCrc(cpu, unit, 0);
    SetKeys(unit, 0, 0);
    const uint32_t length = UnitLength(cpu, unit);
    const uint32_t computed = GuestCall{cpu}.Virtual(unit, vCrc, {length}) & 0xFFFF;

    if (int16_t(crc) != int16_t(computed)) {
        Log(cpu, vError, kMsgUnitCrc, uint32_t(int32_t(int16_t(crc))), uint32_t(int32_t(int16_t(computed))));
        GuestCall{cpu}.Virtual(unit, vReset, {});
        return false;
    }

    const int16_t magic = int16_t(UnitField16(cpu, unit, 0));
    const int16_t expected = int16_t(Memory::Read16(kUnitMagic));

    if (magic == expected)
        return true;

    Log(cpu, vError, kMsgUnitMagic, uint32_t(int32_t(magic)), uint32_t(int32_t(expected)));
    GuestCall{cpu}.Virtual(unit, vReset, {});
    return false;
}

// 0x803FA658: an empty pool.
void PoolConstruct(uint32_t pool) {
    BufferInit(pool);
    Write(pool, kPoolVtable);
    Memory::Write16(pool + bSequence, 0);
    Write(pool + bCursor, 0);
}

// 0x803FA6FC: appends the reader's current packet.
int32_t PoolAppend(CpuContext* cpu, uint32_t pool, uint32_t reader, uint32_t mask) {
    const int32_t copied = CopyPacket(cpu, reader, Read(pool + bData) + Read(pool + bLength), Read(pool + bCapacity) - Read(pool + bLength), mask);

    if (copied >= 0)
        Write(pool + bLength, Read(pool + bLength) + uint32_t(copied));

    return copied;
}

// 0x803FA760: appends every packet left in the reader; answers the bytes
// (or a failed append's answer).
int32_t PoolAppendAll(CpuContext* cpu, uint32_t pool, uint32_t reader) {
    int32_t total = 0;

    while (HasPacket(cpu, reader)) {
        const int32_t copied = PoolAppend(cpu, pool, reader, 0xFF);

        if (copied < 0)
            return copied;

        total += copied;
        ReaderNext(cpu, reader);
    }

    return total;
}

// A reader on the guest stack.
struct StackReader {
    GuestFrame frame;
    uint32_t address;

    StackReader(CpuContext* cpu) : frame(cpu, 0x40), address(frame[0x10]) {
        ReaderConstruct(address);
    }
};

// 0x803FA800: drops packets from the front until `bytes` (counted with the
// header parts `mask` keeps) are gone.
void PoolDrop(CpuContext* cpu, uint32_t pool, int32_t bytes, uint32_t mask) {
    StackReader reader(cpu);
    ReaderInit(cpu, reader.address, Read(pool + bData), Read(pool + bLength));
    uint32_t dropped = 0;

    while (HasPacket(cpu, reader.address)) {
        bytes -= int32_t(PacketSize(cpu, reader.address, mask) & 0xFFFF);
        dropped += PacketSize(cpu, reader.address, 0xFF) & 0xFFFF;

        if (bytes < 1)
            break;

        ReaderNext(cpu, reader.address);
    }

    const int32_t left = ReadInt(pool + bLength) - int32_t(dropped);

    if (left > 0)
        GuestCall{cpu}.Call<Memmove>({Read(pool + bData), Read(pool + bData) + dropped, uint32_t(left)});

    Write(pool + bLength, uint32_t(left));
}

// 0x803FAC9C: two 16-bit sequence numbers, the earlier one moved past the
// wrap when they are closer that way.
void Unwrap(uint32_t a, uint32_t b, int32_t* outA, int32_t* outB) {
    a &= 0xFFFF;
    b &= 0xFFFF;

    if (int32_t(b + 0x10000 - a) < int32_t(a - b))
        b += 0x10000;
    else if (int32_t(a + 0x10000 - b) < int32_t(b - a))
        a += 0x10000;

    *outA = int32_t(a);
    *outB = int32_t(b);
}

// 0x803FA8FC / 0x803FAA34: adds the unit's packets past the pool's
// sequence: 0 when none are new, 2 (top) when the unit starts later, 3 / 1
// when added, kPoolFull without room.
uint32_t PoolPut(CpuContext* cpu, uint32_t pool, uint32_t unit, bool top) {
    StackReader reader(cpu);
    const uint32_t length = UnitPayloadLength(cpu, unit);
    const uint32_t sequence = UnitField16(cpu, unit, 4);
    int32_t mine = 0, theirs = 0;
    Unwrap(Memory::Read16(pool + bSequence), sequence, &mine, &theirs);

    if (mine >= theirs + int32_t(length))
        return 0;

    if (top) {
        if (mine < theirs)
            return 2;

        const int32_t skip = mine - theirs;
        ReaderInit(cpu, reader.address, UnitPayload(cpu, unit) + uint32_t(skip), length - uint32_t(skip));
    } else {
        if (mine > theirs)
            return 0;

        ReaderInit(cpu, reader.address, UnitPayload(cpu, unit), length);
    }

    if (PoolAppendAll(cpu, pool, reader.address) < 0) {
        Log(cpu, vWarn, top ? kMsgPoolPutTop : kMsgPoolPutLow);
        return kPoolFull;
    }

    Memory::Write16(pool + bSequence, uint16_t(theirs + int32_t(length)));
    return top ? 3 : 1;
}

// 0x803FAB60: adds all the unit's packets.
uint32_t PoolPutAll(CpuContext* cpu, uint32_t pool, uint32_t unit) {
    StackReader reader(cpu);
    const uint32_t length = UnitPayloadLength(cpu, unit);
    ReaderInit(cpu, reader.address, UnitPayload(cpu, unit), length);

    if (PoolAppendAll(cpu, pool, reader.address) < 0) {
        Log(cpu, vWarn, kMsgPoolPutAll);
        return kPoolFull;
    }

    return 1;
}

// 0x803FAC24: drops what the unit acknowledges.
void PoolAck(CpuContext* cpu, uint32_t pool, uint32_t unit) {
    const uint32_t ack = UnitField16(cpu, unit, 6);
    int32_t mine = 0, theirs = 0;
    Unwrap(Memory::Read16(pool + bSequence), ack, &mine, &theirs);

    if (mine < theirs) {
        PoolDrop(cpu, pool, theirs - mine, 0xBF);
        Memory::Write16(pool + bSequence, uint16_t(theirs));
    }
}

// A reader over what the pool holds from its cursor on.
void ReaderAtCursor(CpuContext* cpu, uint32_t reader, uint32_t pool) {
    ReaderInit(cpu, reader, Read(pool + bCursor), Remaining(pool, Read(pool + bCursor)));
}

// 0x803FACF0: a packet is at the cursor.
bool PoolHasNext(CpuContext* cpu, uint32_t pool) {
    StackReader reader(cpu);
    ReaderAtCursor(cpu, reader.address, pool);
    return HasPacket(cpu, reader.address);
}

// 0x803FAD58: the cursor past its packet.
void PoolAdvance(CpuContext* cpu, uint32_t pool) {
    StackReader reader(cpu);
    ReaderAtCursor(cpu, reader.address, pool);

    if (HasPacket(cpu, reader.address))
        Write(pool + bCursor, Read(pool + bCursor) + (PacketSize(cpu, reader.address, 0xFF) & 0xFFFF));
}

// 0x803FAE1C: removes the packet at the cursor.
void PoolRemove(CpuContext* cpu, uint32_t pool) {
    StackReader reader(cpu);
    const uint32_t left = Remaining(pool, Read(pool + bCursor));
    ReaderInit(cpu, reader.address, Read(pool + bCursor), left);

    if (!HasPacket(cpu, reader.address))
        return;

    const uint32_t size = PacketSize(cpu, reader.address, 0xFF) & 0xFFFF;
    Write(pool + bLength, Read(pool + bLength) - size);

    if (left != size)
        GuestCall{cpu}.Call<Memmove>({Read(pool + bCursor), Read(pool + bCursor) + size, left - size});
}

// The derived buffers' destructors: the base's, then the heap object.
uint32_t DerivedDestroy(CpuContext* cpu, uint32_t buffer, uint32_t flags) {
    if (buffer) {
        BufferDestroy(cpu, buffer, 0);

        if (int16_t(flags) > 0)
            GuestCall{cpu}.Call<OperatorDelete>({buffer});
    }

    return buffer;
}

uint32_t Arg(CpuContext* cpu, int n) {
    return cpu->gpr[n];
}

uint32_t Answer(int32_t value) {
    return uint32_t(value);
}

} // namespace

extern "C" void MH3_NetBufferInit(CpuContext* cpu) {
    BufferInit(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C98E0, MH3_NetBufferInit, "NetworkBuffer_ctor");

extern "C" void MH3_NetBufferDestroy(CpuContext* cpu) {
    cpu->gpr[3] = BufferDestroy(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9900, MH3_NetBufferDestroy, "NetworkBuffer_dtor");

extern "C" void MH3_NetBufferNoOp(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9944, MH3_NetBufferNoOp, "NetworkBuffer_noop");

extern "C" void MH3_NetBufferReload(CpuContext* cpu) {
    const uint32_t buffer = Arg(cpu, 3);
    GuestCall{cpu}.Virtual(buffer, 0xC, {Read(buffer + bData), Read(buffer + bLength)});
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9948, MH3_NetBufferReload, "NetworkBuffer_reload");

extern "C" void MH3_NetBufferAssign(CpuContext* cpu) {
    BufferAssign(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9960, MH3_NetBufferAssign, "NetworkBuffer_assign");

extern "C" void MH3_NetBufferClear(CpuContext* cpu) {
    BufferClear(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C997C, MH3_NetBufferClear, "NetworkBuffer_init");

extern "C" void MH3_NetPlatform(CpuContext* cpu) {
    cpu->gpr[3] = Platform();
}
REGISTER_NATIVE_FUNCTION_AS(0x803C99D8, MH3_NetPlatform, "NetworkPlatform_get");

#ifdef MH3_DEVELOPMENT
// MH3_TRACE=net: the platform's log hooks (empty on the Wii) print the net
// library's messages through OSReport. The format and the arguments in
// registers go along (floats stay in f1.. with CR bit 6 as the caller set
// them); arguments the caller passed on the stack do not.
bool TraceNetLog() {
    static const TraceFlag trace("net");
    return trace;
}

void ForwardLog(CpuContext* cpu, int format) {
    if (!TraceNetLog())
        return;

    uint32_t args[6] = {};

    for (int i = 0; i < 6 && format + i <= 10; ++i)
        args[i] = cpu->gpr[format + i];

    GuestCall{cpu}.Call<OSReport>({args[0], args[1], args[2], args[3], args[4], args[5]});
}
#endif

// 0x80418F70 / 0x80418FC4 / 0x80419018 (platform virtuals 0xC, 0x10, 0x14):
// info (level, format, ...), error and warning (format, ...).
extern "C" void MH3_NetPlatformInfo(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    ForwardLog(cpu, 5);
#endif
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80418F70, MH3_NetPlatformInfo, "NetworkPlatformWii_info");

extern "C" void MH3_NetPlatformError(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    ForwardLog(cpu, 4);
#endif
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80418FC4, MH3_NetPlatformError, "NetworkPlatformWii_error");

extern "C" void MH3_NetPlatformWarning(CpuContext* cpu) {
#ifdef MH3_DEVELOPMENT
    ForwardLog(cpu, 4);
#endif
    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x80419018, MH3_NetPlatformWarning, "NetworkPlatformWii_warning");

extern "C" void MH3_NetBufferAttach(CpuContext* cpu) {
    BufferAttach(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C99E0, MH3_NetBufferAttach, "NetworkBuffer_attach");

extern "C" void MH3_NetBufferSerialize(CpuContext* cpu) {
    cpu->gpr[3] = Answer(BufferSerialize(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C99F0, MH3_NetBufferSerialize, "NetworkBuffer_serialize");

extern "C" void MH3_NetBufferDeserialize(CpuContext* cpu) {
    cpu->gpr[3] = Answer(BufferDeserialize(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9AF4, MH3_NetBufferDeserialize, "NetworkBuffer_deserialize");

extern "C" void MH3_NetBufferDuplicate(CpuContext* cpu) {
    cpu->gpr[3] = Answer(BufferDuplicate(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9BF4, MH3_NetBufferDuplicate, "NetworkBuffer_duplicate");

extern "C" void MH3_NetBufferEquals(CpuContext* cpu) {
    cpu->gpr[3] = BufferEquals(cpu, Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9D3C, MH3_NetBufferEquals, "NetworkBuffer_equals");

extern "C" void MH3_NetBufferScramble(CpuContext* cpu) {
    Scramble(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9E4C, MH3_NetBufferScramble, "NetworkBuffer_encode");

extern "C" void MH3_NetBufferUnscramble(CpuContext* cpu) {
    Unscramble(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803C9F58, MH3_NetBufferUnscramble, "NetworkBuffer_decode");

extern "C" void MH3_NetBufferCrc16(CpuContext* cpu) {
    cpu->gpr[3] = Crc16(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CA060, MH3_NetBufferCrc16, "NetworkBuffer_crc16");

extern "C" void MH3_NetDestroy803CB960(CpuContext* cpu) {
    cpu->gpr[3] = DerivedDestroy(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB960, MH3_NetDestroy803CB960, "NetworkBufferDerived_dtor_803cb960");

extern "C" void MH3_NetDestroy803CB9BC(CpuContext* cpu) {
    cpu->gpr[3] = DerivedDestroy(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CB9BC, MH3_NetDestroy803CB9BC, "NetworkBufferDerived_dtor_803cb9bc");

extern "C" void MH3_NetDestroy803CCE5C(CpuContext* cpu) {
    cpu->gpr[3] = DerivedDestroy(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CCE5C, MH3_NetDestroy803CCE5C, "NetworkBufferDerived_dtor_803cce5c");

extern "C" void MH3_NetDestroy803FA6A0(CpuContext* cpu) {
    cpu->gpr[3] = DerivedDestroy(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA6A0, MH3_NetDestroy803FA6A0, "NetworkBufferDerived_dtor_803fa6a0");

extern "C" void MH3_NetReaderConstruct(CpuContext* cpu) {
    ReaderConstruct(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBA18, MH3_NetReaderConstruct, "NetworkPacketReader_ctor");

extern "C" void MH3_NetUnitConstruct(CpuContext* cpu) {
    UnitConstruct(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803CBA54, MH3_NetUnitConstruct, "NetworkUnitPacket_ctor");

extern "C" void MH3_NetField14(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + 0x14);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8514, MH3_NetField14, "NetworkObject_field14");

extern "C" void MH3_NetIdIsValid(CpuContext* cpu) {
    cpu->gpr[3] = IdIsValid(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F851C, MH3_NetIdIsValid, "NetworkUniqueIdIsValid");

extern "C" void MH3_NetIdImport(CpuContext* cpu) {
    IdImport(cpu, Arg(cpu, 3), Arg(cpu, 4) & 0xFF, Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8588, MH3_NetIdImport, "NetworkUniqueIdImportFrom");

extern "C" void MH3_NetIdExport(CpuContext* cpu) {
    IdExport(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F86B4, MH3_NetIdExport, "NetworkUniqueIdExportTo");

extern "C" void MH3_NetIdEquals(CpuContext* cpu) {
    cpu->gpr[3] = IdEquals(cpu, Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8764, MH3_NetIdEquals, "NetworkUniqueIdEquals");

extern "C" void MH3_NetIdObjectConstruct(CpuContext* cpu) {
    const uint32_t object = Arg(cpu, 3);
    IdObjectConstruct(cpu, object);
    cpu->gpr[3] = object;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F881C, MH3_NetIdObjectConstruct, "NetworkUniqueId_ctor");

extern "C" void MH3_NetIdObjectIsValid(CpuContext* cpu) {
    cpu->gpr[3] = IdObjectIsValid(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8874, MH3_NetIdObjectIsValid, "NetworkUniqueId_isValid");

extern "C" void MH3_NetIdObjectImport(CpuContext* cpu) {
    IdObjectImport(cpu, Arg(cpu, 3), Arg(cpu, 4) & 0xFF, Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8894, MH3_NetIdObjectImport, "NetworkUniqueId_importFrom");

extern "C" void MH3_NetIdObjectExport(CpuContext* cpu) {
    IdObjectExport(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8914, MH3_NetIdObjectExport, "NetworkUniqueId_exportTo");

extern "C" void MH3_NetIdObjectEquals(CpuContext* cpu) {
    cpu->gpr[3] = IdObjectEquals(cpu, Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8968, MH3_NetIdObjectEquals, "NetworkUniqueId_equals");

extern "C" void MH3_NetWriterAttach(CpuContext* cpu) {
    BufferAttach(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A28, MH3_NetWriterAttach, "NetworkPacketWriter_attach");

extern "C" void MH3_NetWriterEquals(CpuContext* cpu) {
    cpu->gpr[3] = BufferEquals(cpu, Arg(cpu, 3), Arg(cpu, 4)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A2C, MH3_NetWriterEquals, "NetworkPacketWriter_equals");

extern "C" void MH3_NetWriterNoOp(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A30, MH3_NetWriterNoOp, "NetworkPacketWriter_noop");

extern "C" void MH3_NetWriterInit(CpuContext* cpu) {
    const uint32_t writer = Arg(cpu, 3), data = Arg(cpu, 4);
    BufferAssign(cpu, writer, data, Arg(cpu, 5));
    Write(writer + bPacket, data);
    Write(writer + bCursor, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A34, MH3_NetWriterInit, "NetworkPacketWriter_init");

extern "C" void MH3_NetWriterBegin(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriterBegin(cpu, Arg(cpu, 3), Arg(cpu, 4), 1, 0, 0, 0));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A78, MH3_NetWriterBegin, "NetworkPacketWriter_begin");

extern "C" void MH3_NetWriterBeginEx(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriterBegin(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6), Arg(cpu, 7), Arg(cpu, 8)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8A8C, MH3_NetWriterBeginEx, "NetworkPacketWriter_beginEx");

extern "C" void MH3_NetWriteId(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriteId(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8C40, MH3_NetWriteId, "NetworkPacketWriter_writeId");

extern "C" void MH3_NetWriteBytes(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriteBytes(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8CC0, MH3_NetWriteBytes, "NetworkPacketWriter_writeBytes");

extern "C" void MH3_NetWriteU32(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriteU32(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8D2C, MH3_NetWriteU32, "NetworkPacketWriter_writeU32");

extern "C" void MH3_NetWriteU16(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriteU16(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8DC0, MH3_NetWriteU16, "NetworkPacketWriter_writeU16");

extern "C" void MH3_NetWriteU8(CpuContext* cpu) {
    cpu->gpr[3] = Answer(WriteU8(Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8E54, MH3_NetWriteU8, "NetworkPacketWriter_writeU8");

extern "C" void MH3_NetSetPayloadLength(CpuContext* cpu) {
    SetPayloadLength(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8E8C, MH3_NetSetPayloadLength, "NetworkPacket_setLength");

extern "C" void MH3_NetSetTimestamp(CpuContext* cpu) {
    SetTimestamp(cpu, Arg(cpu, 3), cpu->fpr[1].d);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8EEC, MH3_NetSetTimestamp, "NetworkPacket_setTimestamp");

extern "C" void MH3_NetReaderInit(CpuContext* cpu) {
    ReaderInit(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8F84, MH3_NetReaderInit, "NetworkPacketReader_init");

extern "C" void MH3_NetReaderRewind(CpuContext* cpu) {
    ReaderRewind(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F8FD0, MH3_NetReaderRewind, "NetworkPacketReader_rewind");

extern "C" void MH3_NetHasPacket(CpuContext* cpu) {
    cpu->gpr[3] = HasPacket(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9010, MH3_NetHasPacket, "NetworkPacketReader_hasPacket");

extern "C" void MH3_NetReaderNext(CpuContext* cpu) {
    ReaderNext(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F909C, MH3_NetReaderNext, "NetworkPacketReader_next");

extern "C" void MH3_NetCopyPacket(CpuContext* cpu) {
    cpu->gpr[3] = Answer(CopyPacket(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6) & 0xFF));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F913C, MH3_NetCopyPacket, "NetworkPacketReader_copy");

extern "C" void MH3_NetPayload(CpuContext* cpu) {
    cpu->gpr[3] = Payload(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9350, MH3_NetPayload, "NetworkPacketReader_payload");

extern "C" void MH3_NetReadId(CpuContext* cpu) {
    cpu->gpr[3] = ReadId(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F93EC, MH3_NetReadId, "NetworkPacketReader_readId");

extern "C" void MH3_NetReadBytes(CpuContext* cpu) {
    cpu->gpr[3] = ReadBytes(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F947C, MH3_NetReadBytes, "NetworkPacketReader_readBytes");

extern "C" void MH3_NetReadU32(CpuContext* cpu) {
    cpu->gpr[3] = ReadU32(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F94F0, MH3_NetReadU32, "NetworkPacketReader_readU32");

extern "C" void MH3_NetReadU16(CpuContext* cpu) {
    cpu->gpr[3] = ReadU16(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9590, MH3_NetReadU16, "NetworkPacketReader_readU16");

extern "C" void MH3_NetReadU8(CpuContext* cpu) {
    cpu->gpr[3] = ReadU8(Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9630, MH3_NetReadU8, "NetworkPacketReader_readU8");

extern "C" void MH3_NetPacketSize(CpuContext* cpu) {
    cpu->gpr[3] = PacketSize(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F966C, MH3_NetPacketSize, "NetworkPacket_size");

extern "C" void MH3_NetPayloadLength(CpuContext* cpu) {
    cpu->gpr[3] = PayloadLength(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F972C, MH3_NetPayloadLength, "NetworkPacket_payloadLength");

extern "C" void MH3_NetHeaderSize(CpuContext* cpu) {
    cpu->gpr[3] = HeaderSize(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9774, MH3_NetHeaderSize, "NetworkPacket_headerSize");

extern "C" void MH3_NetFlag80(CpuContext* cpu) {
    cpu->gpr[3] = (Flags(Arg(cpu, 3)) & 0x80) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F97FC, MH3_NetFlag80, "NetworkPacket_flag80");

extern "C" void MH3_NetHasTimestamp(CpuContext* cpu) {
    cpu->gpr[3] = HasTimestamp(Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9818, MH3_NetHasTimestamp, "NetworkPacket_hasTimestamp");

extern "C" void MH3_NetTimestamp(CpuContext* cpu) {
    cpu->fpr[1].d = Timestamp(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9834, MH3_NetTimestamp, "NetworkPacket_timestamp");

extern "C" void MH3_NetExtraCount(CpuContext* cpu) {
    cpu->gpr[3] = ExtraCount(Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F98C0, MH3_NetExtraCount, "NetworkPacket_extraCount");

extern "C" void MH3_NetLength16(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + bLength) & 0xFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F98D0, MH3_NetLength16, "NetworkPacket_length16");

extern "C" void MH3_NetMaxHeader(CpuContext* cpu) {
    cpu->gpr[3] = 0x17;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F98DC, MH3_NetMaxHeader, "NetworkPacket_maxHeader");

extern "C" void MH3_NetUnitNoOp(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x803F98E4, MH3_NetUnitNoOp, "NetworkUnitPacket_noop");

extern "C" void MH3_NetUnitInit(CpuContext* cpu) {
    const uint32_t unit = Arg(cpu, 3), data = Arg(cpu, 4);
    BufferAssign(cpu, unit, data, Arg(cpu, 5));
    Write(unit + bPacket, data);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F98E8, MH3_NetUnitInit, "NetworkUnitPacket_init");

extern "C" void MH3_NetUnitBegin(CpuContext* cpu) {
    cpu->gpr[3] = Answer(UnitBegin(cpu, Arg(cpu, 3)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9924, MH3_NetUnitBegin, "NetworkUnitPacket_begin");

extern "C" void MH3_NetUnitAppend(CpuContext* cpu) {
    cpu->gpr[3] = Answer(UnitAppend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5) & 0xFF));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F99D4, MH3_NetUnitAppend, "NetworkUnitPacket_append");

extern "C" void MH3_NetUnitLength16(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + bLength) & 0xFFFF;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9A68, MH3_NetUnitLength16, "NetworkUnitPacket_length16");

extern "C" void MH3_NetUnitSkip(CpuContext* cpu) {
    cpu->gpr[3] = Answer(UnitSkip(Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9A74, MH3_NetUnitSkip, "NetworkUnitPacket_skip");

extern "C" void MH3_NetUnitSet2(CpuContext* cpu) {
    SetUnitField16(cpu, Arg(cpu, 3), 0x2, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9AA4, MH3_NetUnitSet2, "NetworkUnitPacket_setPayloadLength");

extern "C" void MH3_NetUnitSet4(CpuContext* cpu) {
    SetUnitField16(cpu, Arg(cpu, 3), 0x4, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9B08, MH3_NetUnitSet4, "NetworkUnitPacket_setSequence");

extern "C" void MH3_NetUnitSet6(CpuContext* cpu) {
    SetUnitField16(cpu, Arg(cpu, 3), 0x6, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9B6C, MH3_NetUnitSet6, "NetworkUnitPacket_setAck");

extern "C" void MH3_NetUnitSet12(CpuContext* cpu) {
    SetUnitField16(cpu, Arg(cpu, 3), 0x12, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9DEC, MH3_NetUnitSet12, "NetworkUnitPacket_setCrc");

extern "C" void MH3_NetUnitSet8(CpuContext* cpu) {
    SetUnitField32(cpu, Arg(cpu, 3), 0x8, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9BD0, MH3_NetUnitSet8, "NetworkUnitPacket_setSender");

extern "C" void MH3_NetUnitSetC(CpuContext* cpu) {
    SetUnitField32(cpu, Arg(cpu, 3), 0xC, Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9C34, MH3_NetUnitSetC, "NetworkUnitPacket_setReceiver");

extern "C" void MH3_NetUnitSetFlagA(CpuContext* cpu) {
    const uint32_t unit = Arg(cpu, 3), value = Arg(cpu, 4) & 0xFF;

    if (value == 0)
        SetUnitFlags(unit, UnitFlags(unit) | 0x10);
    else if (value == 1)
        SetUnitFlags(unit, UnitFlags(unit) | 0x20);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9C98, MH3_NetUnitSetFlagA, "NetworkUnitPacket_setFlagA");

extern "C" void MH3_NetUnitSetFlagB(CpuContext* cpu) {
    const uint32_t unit = Arg(cpu, 3), value = Arg(cpu, 4) & 0xFF;

    if (value == 0)
        SetUnitFlags(unit, UnitFlags(unit) | 1);
    else if (value == 1)
        SetUnitFlags(unit, UnitFlags(unit) | 2);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9CD4, MH3_NetUnitSetFlagB, "NetworkUnitPacket_setFlagB");

extern "C" void MH3_NetUnitSetFlagC(CpuContext* cpu) {
    const uint32_t unit = Arg(cpu, 3);
    SetUnitFlags(unit, Arg(cpu, 4) ? UnitFlags(unit) | 4 : UnitFlags(unit) & 0xFB);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9D10, MH3_NetUnitSetFlagC, "NetworkUnitPacket_setFlagC");

extern "C" void MH3_NetUnitSeal(CpuContext* cpu) {
    UnitSeal(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9D70, MH3_NetUnitSeal, "NetworkUnitPacket_seal");

extern "C" void MH3_NetUnitSetKeyA(CpuContext* cpu) {
    Memory::Write8(Read(Arg(cpu, 3) + bPacket) + 0x14, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9E50, MH3_NetUnitSetKeyA, "NetworkUnitPacket_setKeyA");

extern "C" void MH3_NetUnitSetKeyB(CpuContext* cpu) {
    Memory::Write8(Read(Arg(cpu, 3) + bPacket) + 0x15, uint8_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9E5C, MH3_NetUnitSetKeyB, "NetworkUnitPacket_setKeyB");

extern "C" void MH3_NetUnitAttach(CpuContext* cpu) {
    const uint32_t unit = Arg(cpu, 3), data = Arg(cpu, 4);
    BufferAttach(unit, data, Arg(cpu, 5));
    Write(unit + bPacket, data);
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9E68, MH3_NetUnitAttach, "NetworkUnitPacket_attach");

extern "C" void MH3_NetUnitHasPacket(CpuContext* cpu) {
    cpu->gpr[3] = UnitHasPacket(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9EA4, MH3_NetUnitHasPacket, "NetworkUnitPacket_hasPacket");

extern "C" void MH3_NetUnitCopy(CpuContext* cpu) {
    cpu->gpr[3] = Answer(UnitCopy(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9F20, MH3_NetUnitCopy, "NetworkUnitPacket_copy");

extern "C" void MH3_NetUnitConsume(CpuContext* cpu) {
    cpu->gpr[3] = UnitConsume(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803F9FD8, MH3_NetUnitConsume, "NetworkUnitPacket_consume");

extern "C" void MH3_NetUnitPayload(CpuContext* cpu) {
    cpu->gpr[3] = UnitPayload(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA07C, MH3_NetUnitPayload, "NetworkUnitPacket_payload");

extern "C" void MH3_NetUnitPacket(CpuContext* cpu) {
    cpu->gpr[3] = Read(Arg(cpu, 3) + bPacket);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA0FC, MH3_NetUnitPacket, "NetworkUnitPacket_packet");

extern "C" void MH3_NetUnitMagic(CpuContext* cpu) {
    cpu->gpr[3] = UnitField16(cpu, Arg(cpu, 3), 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA104, MH3_NetUnitMagic, "NetworkUnitPacket_magic");

extern "C" void MH3_NetUnitLength(CpuContext* cpu) {
    cpu->gpr[3] = UnitLength(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA14C, MH3_NetUnitLength, "NetworkUnitPacket_length");

extern "C" void MH3_NetUnitPayloadLength(CpuContext* cpu) {
    cpu->gpr[3] = UnitPayloadLength(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA174, MH3_NetUnitPayloadLength, "NetworkUnitPacket_payloadLength");

extern "C" void MH3_NetUnitHeaderSize(CpuContext* cpu) {
    cpu->gpr[3] = kUnitHeader;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA1C0, MH3_NetUnitHeaderSize, "NetworkUnitPacket_headerSize");

extern "C" void MH3_NetUnitGet4(CpuContext* cpu) {
    cpu->gpr[3] = UnitField16(cpu, Arg(cpu, 3), 0x4);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA1C8, MH3_NetUnitGet4, "NetworkUnitPacket_sequence");

extern "C" void MH3_NetUnitGet6(CpuContext* cpu) {
    cpu->gpr[3] = UnitField16(cpu, Arg(cpu, 3), 0x6);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA214, MH3_NetUnitGet6, "NetworkUnitPacket_ack");

extern "C" void MH3_NetUnitGet12(CpuContext* cpu) {
    cpu->gpr[3] = UnitField16(cpu, Arg(cpu, 3), 0x12);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA390, MH3_NetUnitGet12, "NetworkUnitPacket_crc");

extern "C" void MH3_NetUnitGet8(CpuContext* cpu) {
    cpu->gpr[3] = UnitField32(cpu, Arg(cpu, 3), 0x8);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA260, MH3_NetUnitGet8, "NetworkUnitPacket_sender");

extern "C" void MH3_NetUnitGetC(CpuContext* cpu) {
    cpu->gpr[3] = UnitField32(cpu, Arg(cpu, 3), 0xC);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA2AC, MH3_NetUnitGetC, "NetworkUnitPacket_receiver");

extern "C" void MH3_NetUnitFlagA(CpuContext* cpu) {
    const uint32_t flags = UnitFlags(Arg(cpu, 3));
    cpu->gpr[3] = (flags & 0x10) ? 0 : (flags & 0x20) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA2F8, MH3_NetUnitFlagA, "NetworkUnitPacket_flagA");

extern "C" void MH3_NetUnitFlagB(CpuContext* cpu) {
    const uint32_t flags = UnitFlags(Arg(cpu, 3));
    cpu->gpr[3] = (flags & 1) ? 0 : (flags & 2) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA328, MH3_NetUnitFlagB, "NetworkUnitPacket_flagB");

extern "C" void MH3_NetUnitFlagC(CpuContext* cpu) {
    cpu->gpr[3] = (UnitFlags(Arg(cpu, 3)) & 4) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA358, MH3_NetUnitFlagC, "NetworkUnitPacket_flagC");

extern "C" void MH3_NetUnitFlagD(CpuContext* cpu) {
    cpu->gpr[3] = (UnitFlags(Arg(cpu, 3)) & 8) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA374, MH3_NetUnitFlagD, "NetworkUnitPacket_flagD");

extern "C" void MH3_NetUnitKeyA(CpuContext* cpu) {
    cpu->gpr[3] = Byte(Read(Arg(cpu, 3) + bPacket) + 0x14);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA3DC, MH3_NetUnitKeyA, "NetworkUnitPacket_keyA");

extern "C" void MH3_NetUnitKeyB(CpuContext* cpu) {
    cpu->gpr[3] = Byte(Read(Arg(cpu, 3) + bPacket) + 0x15);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA3E8, MH3_NetUnitKeyB, "NetworkUnitPacket_keyB");

extern "C" void MH3_NetUnitScramble(CpuContext* cpu) {
    UnitScramble(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA3F4, MH3_NetUnitScramble, "NetworkUnitPacket_encode");

extern "C" void MH3_NetUnitUnscramble(CpuContext* cpu) {
    UnitUnscramble(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA49C, MH3_NetUnitUnscramble, "NetworkUnitPacket_decode");

extern "C" void MH3_NetUnitVerify(CpuContext* cpu) {
    cpu->gpr[3] = UnitVerify(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA514, MH3_NetUnitVerify, "NetworkUnitPacket_test");

extern "C" void MH3_NetPoolConstruct(CpuContext* cpu) {
    const uint32_t pool = Arg(cpu, 3);
    PoolConstruct(pool);
    cpu->gpr[3] = pool;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA658, MH3_NetPoolConstruct, "NetworkUnitPacketPool_ctor");

extern "C" void MH3_NetPoolAppend(CpuContext* cpu) {
    cpu->gpr[3] = Answer(PoolAppend(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5) & 0xFF));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA6FC, MH3_NetPoolAppend, "NetworkUnitPacketPool_append");

extern "C" void MH3_NetPoolAppendAll(CpuContext* cpu) {
    cpu->gpr[3] = Answer(PoolAppendAll(cpu, Arg(cpu, 3), Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA760, MH3_NetPoolAppendAll, "NetworkUnitPacketPool_appendAll");

extern "C" void MH3_NetPoolLoadInto(CpuContext* cpu) {
    const uint32_t pool = Arg(cpu, 3), target = Arg(cpu, 4);
    GuestCall{cpu}.Virtual(target, vLoad, {Read(pool + bData), Read(pool + bLength)});
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA7E0, MH3_NetPoolLoadInto, "NetworkUnitPacketPool_loadInto");

extern "C" void MH3_NetPoolDrop(CpuContext* cpu) {
    PoolDrop(cpu, Arg(cpu, 3), int32_t(Arg(cpu, 4)), Arg(cpu, 5) & 0xFF);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA800, MH3_NetPoolDrop, "NetworkUnitPacketPool_drop");

extern "C" void MH3_NetPoolReset(CpuContext* cpu) {
    Write(Arg(cpu, 3) + bLength, 0);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA8E0, MH3_NetPoolReset, "NetworkUnitPacketPool_reset");

extern "C" void MH3_NetPoolSetSequence(CpuContext* cpu) {
    Memory::Write16(Arg(cpu, 3) + bSequence, uint16_t(Arg(cpu, 4)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA8EC, MH3_NetPoolSetSequence, "NetworkUnitPacketPool_setSequence");

extern "C" void MH3_NetPoolSequence(CpuContext* cpu) {
    cpu->gpr[3] = Memory::Read16(Arg(cpu, 3) + bSequence);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA8F4, MH3_NetPoolSequence, "NetworkUnitPacketPool_sequence");

extern "C" void MH3_NetPoolPutTop(CpuContext* cpu) {
    cpu->gpr[3] = PoolPut(cpu, Arg(cpu, 3), Arg(cpu, 4), true);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FA8FC, MH3_NetPoolPutTop, "NetworkUnitPacketPool_putTopPacket");

extern "C" void MH3_NetPoolPutLow(CpuContext* cpu) {
    cpu->gpr[3] = PoolPut(cpu, Arg(cpu, 3), Arg(cpu, 4), false);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAA34, MH3_NetPoolPutLow, "NetworkUnitPacketPool_putLowPacket");

extern "C" void MH3_NetPoolPutAll(CpuContext* cpu) {
    cpu->gpr[3] = PoolPutAll(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAB60, MH3_NetPoolPutAll, "NetworkUnitPacketPool_putAllPacket");

extern "C" void MH3_NetPoolAck(CpuContext* cpu) {
    PoolAck(cpu, Arg(cpu, 3), Arg(cpu, 4));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAC24, MH3_NetPoolAck, "NetworkUnitPacketPool_ack");

extern "C" void MH3_NetPoolUnwrap(CpuContext* cpu) {
    int32_t a = 0, b = 0;
    Unwrap(Arg(cpu, 4), Arg(cpu, 5), &a, &b);
    Write(Arg(cpu, 6), uint32_t(a));
    Write(Arg(cpu, 7), uint32_t(b));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAC9C, MH3_NetPoolUnwrap, "NetworkUnitPacketPool_unwrap");

extern "C" void MH3_NetPoolRewind(CpuContext* cpu) {
    const uint32_t pool = Arg(cpu, 3);
    Write(pool + bCursor, Read(pool + bData));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FACE4, MH3_NetPoolRewind, "NetworkUnitPacketPool_rewind");

extern "C" void MH3_NetPoolHasNext(CpuContext* cpu) {
    cpu->gpr[3] = PoolHasNext(cpu, Arg(cpu, 3)) ? 1 : 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x803FACF0, MH3_NetPoolHasNext, "NetworkUnitPacketPool_hasNext");

extern "C" void MH3_NetPoolAdvance(CpuContext* cpu) {
    PoolAdvance(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAD58, MH3_NetPoolAdvance, "NetworkUnitPacketPool_next");

extern "C" void MH3_NetPoolLoadAtCursor(CpuContext* cpu) {
    const uint32_t pool = Arg(cpu, 3), target = Arg(cpu, 4);
    GuestCall{cpu}.Virtual(target, vLoad, {Read(pool + bCursor), Remaining(pool, Read(pool + bCursor))});
}
REGISTER_NATIVE_FUNCTION_AS(0x803FADEC, MH3_NetPoolLoadAtCursor, "NetworkUnitPacketPool_loadAtCursor");

extern "C" void MH3_NetPoolRemove(CpuContext* cpu) {
    PoolRemove(cpu, Arg(cpu, 3));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAE1C, MH3_NetPoolRemove, "NetworkUnitPacketPool_remove");

extern "C" void MH3_NetPoolNoOp(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAED4, MH3_NetPoolNoOp, "NetworkUnitPacketPool_noop");

extern "C" void MH3_NetPoolSerialize(CpuContext* cpu) {
    cpu->gpr[3] = Answer(BufferSerialize(cpu, Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5)));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAED8, MH3_NetPoolSerialize, "NetworkUnitPacketPool_serialize");

extern "C" void MH3_NetPoolNoOp2(CpuContext*) {
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAEDC, MH3_NetPoolNoOp2, "NetworkUnitPacketPool_noop2");

extern "C" void MH3_NetPoolScramble(CpuContext* cpu) {
    Scramble(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAEE0, MH3_NetPoolScramble, "NetworkUnitPacketPool_encode");

extern "C" void MH3_NetPoolUnscramble(CpuContext* cpu) {
    Unscramble(Arg(cpu, 3), Arg(cpu, 4), Arg(cpu, 5), Arg(cpu, 6));
}
REGISTER_NATIVE_FUNCTION_AS(0x803FAEF0, MH3_NetPoolUnscramble, "NetworkUnitPacketPool_decode");

extern "C" void MH3_NetPlatformIdentity30(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80418430, MH3_NetPlatformIdentity30, "NetworkPlatformWii_identity_30");

extern "C" void MH3_NetPlatformIdentity38(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80418438, MH3_NetPlatformIdentity38, "NetworkPlatformWii_identity_38");

extern "C" void MH3_NetPlatformIdentity40(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80418440, MH3_NetPlatformIdentity40, "NetworkPlatformWii_identity_40");

extern "C" void MH3_NetPlatformIdentity48(CpuContext* cpu) {
    cpu->gpr[3] = Arg(cpu, 4);
}
REGISTER_NATIVE_FUNCTION_AS(0x80418448, MH3_NetPlatformIdentity48, "NetworkPlatformWii_identity_48");
