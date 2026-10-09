// Native Wii disc image decoder: ISO, WIA and RVZ (docs/rom-decoder.md).
//
// Implements the C bridge in vita/rom_decoder.h for RomImage. It returns the
// bytes the nod 1.4.4 bridge returned for the same calls: disc reads give
// partition sectors decrypted (WIA/RVZ: zeroed hash block, hashes are not
// rebuilt); flat reads give the game partition's data, 0x7c00 bytes per
// sector. The image is read through the caller's 64-bit positioned reads; the
// decoder opens no files and starts no threads.
//
// Formats: WiiBrew's "Wii disc" (partition table, ticket, AES-128-CBC
// sectors) and Dolphin's docs/WiaAndRvz.md (WIA/RVZ headers, groups,
// exception lists, RVZ packing and its junk generator).
#include "../vita/rom_decoder.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <bzlib.h>
#include <zstd.h>
#include "Lzma2Dec.h"
#include "LzmaDec.h"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the junk generator stores big-endian words byte-swapped");

// bzip2 built with BZ_NO_STDIO reports broken internal invariants here.
extern "C" void bz_internal_error(int code) {
    std::fprintf(stderr, "bzip2 internal error %d\n", code);
    std::abort();
}

namespace {
constexpr size_t kSector = 0x8000, kHashes = 0x400, kSectorData = kSector - kHashes;
constexpr uint32_t kWiiMagic = 0x5d1c9ea3, kGameCubeMagic = 0xc2339f3d;

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
void PutBe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// SHA-1 (FIPS 180-4), for the WIA/RVZ header checks.
std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    const auto rotl = [](uint32_t x, int n) { return x << n | x >> (32 - n); };
    const auto block = [&](const uint8_t* p) {
        uint32_t w[80];

        for (int i = 0; i < 16; ++i)
            w[i] = Be32(p + i * 4);

        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;

            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5a827999;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ed9eba1;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
            else
                f = b ^ c ^ d, k = 0xca62c1d6;

            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rotl(b, 30), b = a, a = t;
        }

        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };

    size_t done = 0;

    for (; size - done >= 64; done += 64)
        block(data + done);

    uint8_t tail[128] = {};
    const size_t rest = size - done, tailSize = rest < 56 ? 64 : 128;
    std::memcpy(tail, data + done, rest);
    tail[rest] = 0x80;

    for (int i = 0; i < 8; ++i)
        tail[tailSize - 1 - i] = uint8_t(uint64_t(size) * 8 >> (i * 8));

    block(tail);

    if (tailSize == 128)
        block(tail + 64);

    std::array<uint8_t, 20> digest;

    for (int i = 0; i < 5; ++i)
        PutBe32(digest.data() + i * 4, h[i]);

    return digest;
}

bool HashMatches(const uint8_t* data, size_t size, const uint8_t* expected) {
    return !std::memcmp(Sha1(data, size).data(), expected, 20);
}

// AES-128 decryption (FIPS 197) with 32-bit tables, built at compile time.
constexpr uint8_t GfMul(uint8_t a, uint8_t b) {
    uint8_t product = 0;

    for (; b; b >>= 1) {
        if (b & 1)
            product ^= a;

        a = uint8_t(a << 1 ^ (a & 0x80 ? 0x1b : 0));
    }

    return product;
}

struct AesTables {
    uint8_t sbox[256]{}, inverse[256]{};
    uint32_t decrypt[4][256]{}; // InvSubBytes + InvMixColumns, one per byte position
};

constexpr AesTables MakeAesTables() {
    AesTables t;

    // p walks the multiplicative group by 3, q by its inverse (p * q == 1).
    uint8_t p = 1, q = 1;
    const auto rotl = [](uint8_t x, int n) { return uint8_t(x << n | x >> (8 - n)); };

    do {
        p = uint8_t(p ^ p << 1 ^ (p & 0x80 ? 0x1b : 0));
        q = uint8_t(q ^ q << 1);
        q = uint8_t(q ^ q << 2);
        q = uint8_t(q ^ q << 4);

        if (q & 0x80)
            q ^= 0x09;

        t.sbox[p] = uint8_t(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
    } while (p != 1);
    t.sbox[0] = 0x63;

    for (int i = 0; i < 256; ++i)
        t.inverse[t.sbox[i]] = uint8_t(i);

    for (int i = 0; i < 256; ++i) {
        const uint8_t s = t.inverse[i];
        const uint32_t word = uint32_t(GfMul(s, 14)) << 24 | uint32_t(GfMul(s, 9)) << 16 | uint32_t(GfMul(s, 13)) << 8 | GfMul(s, 11);

        for (int r = 0; r < 4; ++r)
            t.decrypt[r][i] = r ? word >> (8 * r) | word << (32 - 8 * r) : word;
    }

    return t;
}

constexpr AesTables kAes = MakeAesTables();

class Aes128 {
  public:
    void SetKey(const uint8_t* key) {
        uint32_t ek[44];

        for (int i = 0; i < 4; ++i)
            ek[i] = Be32(key + i * 4);

        uint8_t rcon = 1;

        for (int i = 4; i < 44; ++i) {
            uint32_t t = ek[i - 1];

            if (i % 4 == 0) {
                t = (uint32_t(kAes.sbox[t >> 16 & 0xff]) << 24 | uint32_t(kAes.sbox[t >> 8 & 0xff]) << 16 |
                     uint32_t(kAes.sbox[t & 0xff]) << 8 | kAes.sbox[t >> 24]) ^ uint32_t(rcon) << 24;
                rcon = GfMul(rcon, 2);
            }

            ek[i] = ek[i - 4] ^ t;
        }

        // Equivalent inverse cipher: reversed round keys, InvMixColumns on the inner ones.
        for (int round = 0; round <= 10; ++round)
            for (int j = 0; j < 4; ++j) {
                const uint32_t w = ek[(10 - round) * 4 + j];
                rk[round * 4 + j] = round == 0 || round == 10 ? w
                    : kAes.decrypt[0][kAes.sbox[w >> 24]] ^ kAes.decrypt[1][kAes.sbox[w >> 16 & 0xff]] ^
                        kAes.decrypt[2][kAes.sbox[w >> 8 & 0xff]] ^ kAes.decrypt[3][kAes.sbox[w & 0xff]];
            }
    }

    // CBC decryption of whole blocks; `in` may equal `out`.
    void DecryptCbc(const uint8_t* iv, const uint8_t* in, uint8_t* out, size_t size) const {
        uint8_t chain[16], cipher[16];
        std::memcpy(chain, iv, 16);

        for (size_t at = 0; at < size; at += 16) {
            std::memcpy(cipher, in + at, 16);
            DecryptBlock(cipher, out + at);

            for (int i = 0; i < 16; ++i)
                out[at + i] ^= chain[i];

            std::memcpy(chain, cipher, 16);
        }
    }

  private:
    uint32_t rk[44] = {};
    void DecryptBlock(const uint8_t* in, uint8_t* out) const {
        const auto& T = kAes.decrypt;
        uint32_t s0 = Be32(in) ^ rk[0], s1 = Be32(in + 4) ^ rk[1], s2 = Be32(in + 8) ^ rk[2], s3 = Be32(in + 12) ^ rk[3];
        const uint32_t* k = rk + 4;

        for (int round = 1; round < 10; ++round, k += 4) {
            const uint32_t t0 = T[0][s0 >> 24] ^ T[1][s3 >> 16 & 0xff] ^ T[2][s2 >> 8 & 0xff] ^ T[3][s1 & 0xff] ^ k[0];
            const uint32_t t1 = T[0][s1 >> 24] ^ T[1][s0 >> 16 & 0xff] ^ T[2][s3 >> 8 & 0xff] ^ T[3][s2 & 0xff] ^ k[1];
            const uint32_t t2 = T[0][s2 >> 24] ^ T[1][s1 >> 16 & 0xff] ^ T[2][s0 >> 8 & 0xff] ^ T[3][s3 & 0xff] ^ k[2];
            const uint32_t t3 = T[0][s3 >> 24] ^ T[1][s2 >> 16 & 0xff] ^ T[2][s1 >> 8 & 0xff] ^ T[3][s0 & 0xff] ^ k[3];
            s0 = t0, s1 = t1, s2 = t2, s3 = t3;
        }

        const auto* I = kAes.inverse;
        const auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
            return uint32_t(I[a >> 24]) << 24 | uint32_t(I[b >> 16 & 0xff]) << 16 | uint32_t(I[c >> 8 & 0xff]) << 8 | I[d & 0xff];
        };

        PutBe32(out, last(s0, s3, s2, s1) ^ k[0]);
        PutBe32(out + 4, last(s1, s0, s3, s2) ^ k[1]);
        PutBe32(out + 8, last(s2, s1, s0, s3) ^ k[2]);
        PutBe32(out + 12, last(s3, s2, s1, s0) ^ k[3]);
    }
};

// The lagged Fibonacci generator of Wii junk data, seeded from RVZ packing.
class JunkGenerator {
  public:
    void Seed(const uint8_t* seed) { // 17 big-endian words
        for (size_t i = 0; i < 17; ++i)
            words[i] = Be32(seed + i * 4);

        for (size_t i = 17; i < kWords; ++i)
            words[i] = words[i - 17] << 23 ^ words[i - 16] >> 9 ^ words[i - 1];
        // Output bits 16..23 come from bits 18..25; words are kept as their
        // big-endian output bytes (the step below only XORs them).
        for (auto& w : words)
            w = __builtin_bswap32((w & 0xff00ffff) | (w >> 2 & 0x00ff0000));

        position = 0;

        for (int i = 0; i < 4; ++i)
            Forward();
    }

    void Skip(size_t count) {
        for (position += count; position >= kBytes; position -= kBytes)
            Forward();
    }

    void Fill(uint8_t* out, size_t count) {
        while (count) {
            const size_t n = std::min(count, kBytes - position);
            std::memcpy(out, reinterpret_cast<const uint8_t*>(words) + position, n);
            out += n, count -= n, position += n;

            if (position == kBytes)
                Forward(), position = 0;
        }
    }

  private:
    static constexpr size_t kWords = 521, kLag = 32, kBytes = kWords * 4;
    uint32_t words[kWords] = {};
    size_t position = 0;
    void Forward() {
        for (size_t i = 0; i < kLag; ++i)
            words[i] ^= words[i + kWords - kLag];

        for (size_t i = kLag; i < kWords; ++i)
            words[i] ^= words[i - kLag];
    }
};

// WIA/RVZ compression methods.
enum : uint32_t { kNone = 0, kPurge = 1, kBzip2 = 2, kLzma = 3, kLzma2 = 4, kZstd = 5 };

void* LzmaAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void LzmaFree(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kLzmaAlloc{LzmaAlloc, LzmaFree};

class Decompressor {
  public:
    // Null, with `error` set, when the method or its properties are not
    // supported.
    static std::unique_ptr<Decompressor> Open(uint32_t method, const uint8_t* props, size_t propsSize, std::string& error) {
        std::unique_ptr<Decompressor> d(new Decompressor(method));

        if (method == kZstd) {
            d->zstd = ZSTD_createDCtx();

            if (!d->zstd)
                return error = "Out of memory", nullptr;
        } else if (method == kLzma) {
            // 7-Zip SDK properties: lc/lp/pb byte, then the dictionary size.
            LzmaDec_Construct(&d->lzma);

            if (propsSize != LZMA_PROPS_SIZE || LzmaDec_AllocateProbs(&d->lzma, props, LZMA_PROPS_SIZE, &kLzmaAlloc) != SZ_OK)
                return error = "Invalid LZMA properties", nullptr;
        } else if (method == kLzma2) {
            Lzma2Dec_Construct(&d->lzma2);

            if (propsSize != 1 || Lzma2Dec_AllocateProbs(&d->lzma2, props[0], &kLzmaAlloc) != SZ_OK)
                return error = "Invalid LZMA2 properties", nullptr;
        } else if (method == kPurge)
            return error = "Unsupported WIA/RVZ compression: Purge", nullptr;
        else if (method != kNone && method != kBzip2)
            return error = "Invalid WIA/RVZ compression type " + std::to_string(method), nullptr;

        return d;
    }

    ~Decompressor() {
        ZSTD_freeDCtx(zstd);

        if (method == kLzma)
            LzmaDec_FreeProbs(&lzma, &kLzmaAlloc);
        else if (method == kLzma2)
            Lzma2Dec_FreeProbs(&lzma2, &kLzmaAlloc);
    }

    Decompressor(const Decompressor&) = delete;

    // Decompresses one stream (zstd: its frames) into `out`, growing it, and
    // stops at the stream's end or after `limit` bytes. Sets `produced` to the
    // bytes produced (out.size() is only the buffer's capacity), or `error`
    // and returns false when the data is corrupt.
    bool Run(const uint8_t* in, size_t size, std::vector<uint8_t>& out, size_t limit, size_t& produced, std::string& error) {
        produced = 0;
        const auto room = [&] {
            if (produced < out.size())
                return true;

            if (out.size() >= limit)
                return false;

            out.resize(std::min(limit, std::max<size_t>(out.size() * 2, 0x20000)));
            return true;
        };

        if (method == kNone) {
            produced = std::min(size, limit);

            if (out.size() < produced)
                out.resize(produced);

            std::memcpy(out.data(), in, produced);
        } else if (method == kZstd) {
            ZSTD_DCtx_reset(zstd, ZSTD_reset_session_only);
            ZSTD_inBuffer input{in, size, 0};

            while (room()) {
                ZSTD_outBuffer output{out.data(), out.size(), produced};
                const size_t result = ZSTD_decompressStream(zstd, &output, &input);

                if (ZSTD_isError(result))
                    return error = std::string("Zstandard: ") + ZSTD_getErrorName(result), false;

                produced = output.pos;

                if (input.pos == input.size && (result == 0 || output.pos < output.size)) {
                    if (result)
                        return error = "Zstandard data is truncated", false;

                    break;
                }
            }
        } else if (method == kBzip2) {
            bz_stream stream{};

            if (BZ2_bzDecompressInit(&stream, 0, 0) != BZ_OK)
                return error = "Cannot start the bzip2 decoder", false;

            struct End {
                bz_stream* stream;
                ~End() { BZ2_bzDecompressEnd(stream); }
            } end{&stream};
            size_t consumed = 0;

            while (room()) {
                stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(in + consumed));
                stream.avail_in = unsigned(std::min<size_t>(size - consumed, UINT_MAX));
                stream.next_out = reinterpret_cast<char*>(out.data() + produced);
                stream.avail_out = unsigned(std::min<size_t>(out.size() - produced, UINT_MAX));
                const unsigned inBefore = stream.avail_in, outBefore = stream.avail_out;
                const int result = BZ2_bzDecompress(&stream);
                consumed += inBefore - stream.avail_in;
                produced += outBefore - stream.avail_out;

                if (result == BZ_STREAM_END)
                    break;

                if (result != BZ_OK)
                    return error = "bzip2 data is corrupt", false;

                if (consumed == size && produced < out.size())
                    return error = "bzip2 data is truncated", false;
            }
        } else {
            // The output buffer is the dictionary: it holds the whole stream.
            CLzmaDec& dictionary = method == kLzma ? lzma : lzma2.decoder;

            if (method == kLzma)
                LzmaDec_Init(&lzma);
            else
                Lzma2Dec_Init(&lzma2);

            size_t consumed = 0;

            while (room()) {
                dictionary.dic = out.data();
                dictionary.dicBufSize = out.size();
                SizeT length = size - consumed;
                ELzmaStatus status;
                const SRes result = method == kLzma
                    ? LzmaDec_DecodeToDic(&lzma, out.size(), in + consumed, &length, LZMA_FINISH_ANY, &status)
                    : Lzma2Dec_DecodeToDic(&lzma2, out.size(), in + consumed, &length, LZMA_FINISH_ANY, &status);
                consumed += length;
                produced = dictionary.dicPos;

                if (result != SZ_OK)
                    return error = "LZMA data is corrupt", false;
                // Without an end mark the stream ends with its input.
                if (status == LZMA_STATUS_FINISHED_WITH_MARK || status == LZMA_STATUS_NEEDS_MORE_INPUT ||
                    status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
                    break;
            }
        }

        return true;
    }

  private:
    explicit Decompressor(uint32_t method) : method(method) {}

    uint32_t method;
    ZSTD_DCtx* zstd = nullptr;
    CLzmaDec lzma{};
    CLzma2Dec lzma2{};
};

struct Partition {
    uint32_t kind = 0;
    uint32_t dataStart = 0, dataEnd = 0; // sectors
    Aes128 key;                          // decrypted title key
};

// Wii common keys (retail and debug PKI), for the ticket's title key.
constexpr uint8_t kCommonKeys[2][3][16] = {
    {{0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4, 0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7},
     {0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e, 0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e},
     {0x30, 0xbf, 0xc7, 0x6e, 0x7c, 0x19, 0xaf, 0xbb, 0x23, 0x16, 0x33, 0x30, 0xce, 0xd7, 0xc2, 0x8d}},
    {{0xa1, 0x60, 0x4a, 0x6a, 0x71, 0x23, 0xb5, 0x29, 0xae, 0x8b, 0xec, 0x32, 0xc8, 0x16, 0xfc, 0xaa},
     {0x67, 0x45, 0x8b, 0x6b, 0xc6, 0x23, 0x7b, 0x32, 0x69, 0x98, 0x3c, 0x64, 0x73, 0x48, 0x33, 0x66},
     {0x2f, 0x5c, 0x1b, 0x29, 0x44, 0xe7, 0xfd, 0x6f, 0xc3, 0x97, 0x96, 0x4b, 0x05, 0x76, 0x91, 0xfa}}};

class RomReader {
  public:
    RomReader(void* context, uint64_t size, MH3RomReadAt readAt) : context(context), readAt(readAt), fileBytes(size) {}

    // Every step returns false on a failure, which leaves its message here.
    mutable std::string error;

    bool Open();
    uint64_t DiscBytes() const { return discBytes; }
    uint64_t FlatBytes() const { return flatBytes; }
    bool ReadDisc(uint64_t at, uint8_t* out, size_t count);
    bool ReadFlat(uint64_t at, uint8_t* out, size_t count);

  private:
    struct WiaData {
        uint32_t first = 0, count = 0, group = 0, groups = 0; // sectors, then groups
        bool Contains(uint32_t sector) const { return sector >= first && uint64_t(sector) < uint64_t(first) + count; }
    };

    struct WiaRaw {
        uint64_t offset = 0, size = 0;
        uint32_t group = 0, groups = 0;
    };

    struct Group {
        uint64_t offset = 0;
        uint32_t size = 0, packed = 0; // packed: RVZ packing in use
        bool compressed = false;
    };

    struct Slot {
        uint32_t group = UINT32_MAX;
        uint64_t used = 0;
        std::vector<uint8_t> bytes;
        size_t begin = 0, size = 0;
    };

    struct Span {
        const uint8_t* data; // null: zeroes
        size_t size;
    };

    void* context;
    MH3RomReadAt readAt;
    uint64_t fileBytes;
    bool wia = false, plain = false; // plain: ISO partitions stored unencrypted
    uint64_t discBytes = 0, flatBytes = 0;
    std::vector<Partition> partitions;
    size_t game = 0;

    // WIA/RVZ
    uint32_t method = kNone, chunkBytes = 0;
    std::unique_ptr<Decompressor> decompressor;
    std::vector<std::array<WiaData, 2>> wiaPartitions;
    std::vector<WiaRaw> wiaRaw;
    std::vector<Group> groups;
    std::vector<Slot> slots;
    uint64_t clock = 0;
    std::vector<uint8_t> input, stream;
    JunkGenerator junk;

    // One decoded sector of each kind: a disc sector, an ISO partition data sector.
    std::vector<uint8_t> discSector, dataSector, batch;
    uint32_t discSectorIndex = UINT32_MAX, dataSectorIndex = UINT32_MAX;

    bool Fail(std::string message) const {
        error = std::move(message);
        return false;
    }

    bool Source(uint64_t at, uint8_t* out, size_t count) const;
    bool IsoRaw(uint64_t at, uint8_t* out, size_t count) const;
    bool OpenWia(const uint8_t* header);
    bool Table(uint64_t at, uint32_t bytes, size_t needed, std::vector<uint8_t>& table);
    bool ReadPartitions();
    bool SkipExceptions(const uint8_t* data, size_t size, bool partition, size_t& at) const;
    bool LoadGroup(uint32_t index, bool partition, uint64_t junkOffset, Span& span);
    bool WiaSectors(size_t index, uint32_t sector, Span& span);
    bool IsoSectors(const Partition& p, uint32_t sector, uint8_t* out, size_t limit, size_t& count);
    bool DiscSector(uint32_t sector, const uint8_t*& out);
};

bool RomReader::Open() {
    uint8_t header[0x124];

    if (!Source(0, header, sizeof header))
        return false;

    if (!std::memcmp(header, "RVZ\1", 4) || !std::memcmp(header, "WIA\1", 4)) {
        if (!OpenWia(header))
            return false;
    } else if (!std::memcmp(header, "CISO", 4) || !std::memcmp(header, "WBFS", 4) || Be32(header) == 0x01c00bb1 ||
               Be32(header) == 0xae0f38a2 || !std::memcmp(header, "EGGS", 4))
        return Fail("Unsupported disc image format (CISO, GCZ, WBFS, TGC or NFS); convert it to RVZ or ISO");
    else if (Be32(header + 0x18) == kWiiMagic || Be32(header + 0x1c) == kGameCubeMagic)
        discBytes = fileBytes;
    else
        return Fail("Unknown disc image format");

    uint8_t disc[0x80];

    if (!ReadDisc(0, disc, sizeof disc))
        return false;

    if (Be32(disc + 0x18) == kWiiMagic) {
        plain = !wia && disc[0x61] == 1; // no_partition_encryption

        if (!ReadPartitions())
            return false;
    }

    size_t data = 0;

    for (size_t i = 0; i < partitions.size(); ++i)
        if (partitions[i].kind == 0)
            ++data, game = i;
    if (data != 1)
        return Fail("Expected exactly one Wii data partition");

    flatBytes = uint64_t(partitions[game].dataEnd - partitions[game].dataStart) * kSectorData;
    return true;
}

bool RomReader::Source(uint64_t at, uint8_t* out, size_t count) const {
    if (at > fileBytes || count > fileBytes - at)
        return Fail("The disc image is truncated");
    while (count) {
        const intptr_t n = readAt(context, at, out, count);

        if (n <= 0 || size_t(n) > count)
            return Fail("Disc image read failed");

        at += n, out += n, count -= size_t(n);
    }

    return true;
}

// ISO bytes; the image reads as zeroes past its end.
bool RomReader::IsoRaw(uint64_t at, uint8_t* out, size_t count) const {
    const size_t present = at < fileBytes ? size_t(std::min<uint64_t>(count, fileBytes - at)) : 0;

    if (!Source(at, out, present))
        return false;

    std::memset(out + present, 0, count - present);
    return true;
}

bool RomReader::OpenWia(const uint8_t* header) {
    // These limits bound every allocation below (Vita memory) and cover the
    // usual 128 KiB / 2 MiB chunks.
    if (Be32(header + 0x0c) > 0x1000 || Be32(header + 0x54) < 0x8000 || Be32(header + 0x54) > 0x200000 ||
        Be32(header + 0xd8) > 32 || Be32(header + 0xfc) > 16384 || Be32(header + 0x10c) > 131072 ||
        Be32(header + 0x108) > 0x100000 || Be32(header + 0x118) > 0x400000)
        return Fail("RVZ metadata/chunk size exceeds the Vita processing limit");
    wia = true;
    const bool rvz = header[0] == 'R';

    if (!HashMatches(header, 0x34, header + 0x34))
        return Fail("WIA/RVZ file header hash mismatch");

    if (Be32(header + 0x08) < 0x30000)
        return Fail("WIA/RVZ version is not supported");

    discBytes = Be64(header + 0x24);
    std::vector<uint8_t> disc(Be32(header + 0x0c));

    if (!Source(0x48, disc.data(), disc.size()))
        return false;

    if (!HashMatches(disc.data(), disc.size(), header + 0x10))
        return Fail("WIA/RVZ disc header hash mismatch");

    disc.resize(std::max<size_t>(disc.size(), 0xdc));
    const uint8_t* d = disc.data();

    if (Be32(d) != 1 && Be32(d) != 2)
        return Fail("Invalid WIA/RVZ disc type");

    if (Be32(d + 0x94) != 0x30)
        return Fail("Unexpected WIA/RVZ partition entry size");

    method = Be32(d + 0x04);
    chunkBytes = Be32(d + 0x0c);

    if (chunkBytes % kSector)
        return Fail("WIA/RVZ chunk size is not a multiple of 32 KiB");

    if (d[0xd4] > 7)
        return Fail("Invalid WIA/RVZ compressor data");

    decompressor = Decompressor::Open(method, d + 0xd5, d[0xd4], error);

    if (!decompressor)
        return false;

    std::vector<uint8_t> entries(size_t(Be32(d + 0x90)) * 0x30);

    if (!Source(Be64(d + 0x98), entries.data(), entries.size()))
        return false;

    if (!HashMatches(entries.data(), entries.size(), d + 0xa0))
        return Fail("WIA/RVZ partition table hash mismatch");

    for (size_t at = 0; at < entries.size(); at += 0x30) {
        auto& part = wiaPartitions.emplace_back();

        for (int i = 0; i < 2; ++i) {
            const uint8_t* e = entries.data() + at + 0x10 + i * 0x10;
            part[i] = {Be32(e), Be32(e + 4), Be32(e + 8), Be32(e + 12)};
        }
    }

    const uint32_t rawCount = Be32(d + 0xb4);
    std::vector<uint8_t> raw;

    if (!Table(Be64(d + 0xb8), Be32(d + 0xc0), size_t(rawCount) * 0x18, raw))
        return false;

    for (uint32_t i = 0; i < rawCount; ++i) {
        const uint8_t* e = raw.data() + i * 0x18;
        const WiaRaw entry{Be64(e), Be64(e + 8), Be32(e + 16), Be32(e + 20)};

        if ((entry.offset + entry.size) % kSector)
            return Fail("WIA/RVZ raw data is not aligned to sectors");

        wiaRaw.push_back(entry);
    }

    const uint32_t groupCount = Be32(d + 0xc4), entryBytes = rvz ? 12 : 8;
    std::vector<uint8_t> table;

    if (!Table(Be64(d + 0xc8), Be32(d + 0xd0), size_t(groupCount) * entryBytes, table))
        return false;

    groups.resize(groupCount);

    for (uint32_t i = 0; i < groupCount; ++i) {
        const uint8_t* e = table.data() + i * entryBytes;
        const uint32_t size = Be32(e + 4);

        // A WIA group is always stored with the file's compression.
        groups[i] = rvz ? Group{uint64_t(Be32(e)) * 4, size & 0x7fffffff, Be32(e + 8), (size >> 31) != 0}
                        : Group{uint64_t(Be32(e)) * 4, size, 0, true};
    }

    // Cache up to 1 MiB of groups (128 KiB chunks: four).
    slots.resize(std::clamp<size_t>(0x100000 / chunkBytes, 1, 4));
    return true;
}

// A compressed WIA/RVZ header table.
bool RomReader::Table(uint64_t at, uint32_t bytes, size_t needed, std::vector<uint8_t>& table) {
    std::vector<uint8_t> packed(bytes);
    size_t produced = 0;

    if (!Source(at, packed.data(), packed.size()) || !decompressor->Run(packed.data(), packed.size(), table, needed, produced, error))
        return false;

    if (produced < needed)
        return Fail("WIA/RVZ header table is truncated");

    return true;
}

bool RomReader::ReadPartitions() {
    std::vector<Partition> found;
    uint8_t table[32];

    if (!ReadDisc(0x40000, table, sizeof table))
        return false;

    for (int g = 0; g < 4; ++g) {
        const uint32_t count = Be32(table + g * 8);

        if (count > 256)
            return Fail("Invalid Wii partition table");

        std::vector<uint8_t> entries(count * 8);

        if (!ReadDisc(uint64_t(Be32(table + g * 8 + 4)) << 2, entries.data(), entries.size()))
            return false;

        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t offset = uint64_t(Be32(entries.data() + i * 8)) << 2;
            uint8_t h[0x2c0];

            if (!ReadDisc(offset, h, sizeof h))
                return false;

            // Ticket: issuer at 0x140, encrypted title key at 0x1bf, title ID at 0x1dc, common key index at 0x1f1.
            const auto* issuerBytes = reinterpret_cast<const char*>(h + 0x140);
            const std::string_view issuer(issuerBytes, std::find(issuerBytes, issuerBytes + 64, '\0') - issuerBytes);
            const int pki = issuer == "Root-CA00000001-XS00000003" ? 0 : issuer == "Root-CA00000002-XS00000006" ? 1 : -1;

            if (pki < 0 || issuer.size() == 64)
                return Fail("Unknown Wii ticket issuer " + std::string(issuer));

            if (h[0x1f1] >= 3)
                return Fail("Unknown Wii common key index " + std::to_string(h[0x1f1]));

            Aes128 common;
            common.SetKey(kCommonKeys[pki][h[0x1f1]]);
            uint8_t iv[16] = {}, titleKey[16];
            std::memcpy(iv, h + 0x1dc, 8);
            common.DecryptCbc(iv, h + 0x1bf, titleKey, 16);
            const uint64_t dataStart = offset + (uint64_t(Be32(h + 0x2b8)) << 2);
            const uint64_t dataEnd = dataStart + (uint64_t(Be32(h + 0x2bc)) << 2);

            if (offset % kSector || dataStart % kSector || dataEnd % kSector || dataEnd / kSector >= UINT32_MAX)
                return Fail("Wii partition is not sector aligned");

            Partition& p = found.emplace_back();
            p.kind = Be32(entries.data() + i * 8 + 4);
            p.dataStart = uint32_t(dataStart / kSector), p.dataEnd = uint32_t(dataEnd / kSector);
            p.key.SetKey(titleKey);
        }
    }

    // The table was read with no partitions known; sectors now decode differently.
    partitions = std::move(found);
    discSectorIndex = UINT32_MAX;
    return true;
}

// Skips a group's exception lists (Wii partition data: one per 2 MiB) and
// sets `at` past them; hashes are not rebuilt, so the exceptions themselves
// are not needed.
bool RomReader::SkipExceptions(const uint8_t* data, size_t size, bool partition, size_t& at) const {
    at = 0;

    for (uint32_t list = 0; partition && list < (chunkBytes + 0x1fffff) / 0x200000; ++list) {
        if (size - at < 2)
            return Fail("WIA/RVZ exception list is truncated");

        const size_t count = Be16(data + at);
        at += 2;

        if ((size - at) / 22 < count)
            return Fail("WIA/RVZ exception list is truncated");

        at += count * 22;
    }

    return true;
}

bool RomReader::LoadGroup(uint32_t index, bool partition, uint64_t junkOffset, Span& span) {
    if (index >= groups.size())
        return Fail("Missing WIA/RVZ group " + std::to_string(index));

    const Group& g = groups[index];

    if (!g.size) {
        span = {nullptr, 0};
        return true;
    }

    Slot* slot = &slots[0];

    for (auto& s : slots) {
        if (s.group == index) {
            s.used = ++clock;
            span = {s.bytes.data() + s.begin, s.size};
            return true;
        }

        if (s.used < slot->used)
            slot = &s;
    }

    slot->group = UINT32_MAX;
    const size_t limit = size_t(chunkBytes) * 2 + 0x100000;

    if (g.size > limit)
        return Fail("WIA/RVZ group is too large");

    if (input.size() < g.size)
        input.resize(g.size);

    if (!Source(g.offset, input.data(), g.size))
        return false;

    // Uncompressed data keeps its exception lists in front, padded to 4 bytes.
    const bool compressed = g.compressed && method != kNone;
    const bool outerExceptions = !compressed;
    size_t start = 0;

    if (outerExceptions) {
        if (!SkipExceptions(input.data(), g.size, partition, start))
            return false;

        start = (start + 3) & ~size_t(3);
    }

    if (start > g.size)
        return Fail("WIA/RVZ group is truncated");

    std::vector<uint8_t>& owner = compressed ? stream : input;
    size_t size = g.size, at = start;

    if (compressed && !decompressor->Run(input.data() + start, g.size - start, stream, limit, size, error))
        return false;

    if (!outerExceptions && !SkipExceptions(stream.data(), size, partition, at))
        return false;

    const uint8_t* data = owner.data();

    if (!g.packed) {
        std::swap(slot->bytes, owner);
        slot->begin = at, slot->size = size - at;
    } else {
        // RVZ packing: big-endian sizes, each followed by data, or (top bit)
        // by a junk generator seed.
        if (slot->bytes.size() < chunkBytes)
            slot->bytes.resize(chunkBytes);

        size_t produced = 0;

        while (size - at >= 4) {
            uint32_t n = Be32(data + at);
            at += 4;
            const bool isJunk = n >> 31;
            n &= 0x7fffffff;

            if (n > chunkBytes - produced)
                return Fail("RVZ packed data exceeds its group");

            if (isJunk) {
                if (size - at < 68)
                    return Fail("RVZ junk seed is truncated");

                junk.Seed(data + at);
                at += 68;
                junk.Skip((junkOffset + produced) % kSector);
                junk.Fill(slot->bytes.data() + produced, n);
            } else {
                if (size - at < n)
                    return Fail("RVZ packed data is truncated");

                std::memcpy(slot->bytes.data() + produced, data + at, n);
                at += n;
            }

            produced += n;
        }

        slot->begin = 0, slot->size = produced;
    }

    slot->group = index;
    slot->used = ++clock;
    span = {slot->bytes.data() + slot->begin, slot->size};
    return true;
}

// The data of WIA partition sector `sector` (absolute) and of the sectors
// that follow it in the same group: size is a multiple of 0x7c00.
bool RomReader::WiaSectors(size_t index, uint32_t sector, Span& span) {
    if (index >= wiaPartitions.size())
        return Fail("Missing WIA/RVZ partition " + std::to_string(index));

    const auto& part = wiaPartitions[index];
    const Partition& p = partitions[index];

    if (p.dataStart != part[0].first || uint64_t(p.dataEnd) != uint64_t(part[1].first) + part[1].count)
        return Fail("WIA/RVZ partition sectors differ from the disc's partition table");

    const WiaData* pd = part[0].Contains(sector) ? &part[0] : part[1].Contains(sector) ? &part[1] : nullptr;

    if (!pd)
        return Fail("Missing WIA/RVZ partition data for sector " + std::to_string(sector));

    const uint32_t perChunk = chunkBytes / kSector, local = sector - pd->first;
    const uint32_t group = local / perChunk, within = local % perChunk;

    if (group >= pd->groups)
        return Fail("WIA/RVZ partition group is out of range");
    // Junk offsets count partition data bytes from the partition's first data sector.
    const uint64_t junkOffset = uint64_t(group * perChunk + pd->first - part[0].first) * kSectorData;
    Span loaded{};

    if (!LoadGroup(pd->group + group, true, junkOffset, loaded))
        return false;

    size_t sectors = std::min(perChunk - within, pd->count - local);

    if (!loaded.data) {
        span = {nullptr, sectors * kSectorData};
        return true;
    }

    const size_t start = within * kSectorData;

    if (loaded.size < start + kSectorData)
        return Fail("WIA/RVZ group data is truncated");

    sectors = std::min<size_t>(sectors, (loaded.size - start) / kSectorData);
    span = {loaded.data + start, sectors * kSectorData};
    return true;
}

// Decodes whole ISO partition data sectors, at most `limit`, into `out`, and
// sets `count` to their number.
bool RomReader::IsoSectors(const Partition& p, uint32_t sector, uint8_t* out, size_t limit, size_t& count) {
    count = std::min<size_t>({limit, 32, p.dataEnd - sector});
    batch.resize(count * kSector);

    if (!IsoRaw(uint64_t(sector) * kSector, batch.data(), batch.size()))
        return false;

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* raw = batch.data() + i * kSector;
        uint8_t* data = out + i * kSectorData;

        if (uint64_t(sector + i) * kSector >= fileBytes)
            std::memset(data, 0, kSectorData);
        else if (plain)
            std::memcpy(data, raw + kHashes, kSectorData);
        else // the data's IV is the encrypted hash block's bytes 0x3d0..0x3df
            p.key.DecryptCbc(raw + 0x3d0, raw + kHashes, data, kSectorData);
    }

    return true;
}

bool RomReader::DiscSector(uint32_t sector, const uint8_t*& result) {
    if (sector == discSectorIndex) {
        result = discSector.data();
        return true;
    }

    discSectorIndex = UINT32_MAX;
    discSector.resize(kSector);
    uint8_t* out = discSector.data();
    size_t index = 0;

    while (index < partitions.size() && (sector < partitions[index].dataStart || sector >= partitions[index].dataEnd))
        ++index;

    const Partition* part = index < partitions.size() ? &partitions[index] : nullptr;

    if (!wia) {
        const uint64_t at = uint64_t(sector) * kSector;

        if (!IsoRaw(at, out, kSector))
            return false;

        if (part && at < fileBytes && !plain) {
            // Partition sectors read decrypted, hash block included.
            uint8_t iv[16];
            std::memcpy(iv, out + 0x3d0, 16);
            static constexpr uint8_t zero[16] = {};
            part->key.DecryptCbc(zero, out, out, kHashes);
            part->key.DecryptCbc(iv, out + kHashes, out + kHashes, kSectorData);
        }
    } else if (part) {
        Span span{};

        if (!WiaSectors(index, sector, span))
            return false;

        std::memset(out, 0, kHashes);

        if (span.data)
            std::memcpy(out + kHashes, span.data, kSectorData);
        else
            std::memset(out + kHashes, 0, kSectorData);
    } else {
        const auto raw = std::find_if(wiaRaw.begin(), wiaRaw.end(), [&](const WiaRaw& r) {
            return sector >= (r.offset & ~uint64_t(kSector - 1)) / kSector && sector < (r.offset + r.size) / kSector;
        });

        if (raw == wiaRaw.end())
            return Fail("Missing WIA/RVZ raw data for sector " + std::to_string(sector));

        const uint32_t perChunk = chunkBytes / kSector, local = sector - uint32_t(raw->offset / kSector);

        if (local / perChunk >= raw->groups)
            return Fail("WIA/RVZ raw data group is out of range");

        Span span{};

        if (!LoadGroup(raw->group + local / perChunk, false, 0, span))
            return false;

        const size_t start = local % perChunk * kSector;

        if (!span.data)
            std::memset(out, 0, kSector);
        else if (span.size < start + kSector)
            return Fail("WIA/RVZ group data is truncated");
        else
            std::memcpy(out, span.data + start, kSector);
    }

    discSectorIndex = sector;
    result = out;
    return true;
}

bool RomReader::ReadDisc(uint64_t at, uint8_t* out, size_t count) {
    while (count) {
        if (at / kSector >= UINT32_MAX)
            return Fail("Read beyond the disc");

        const uint8_t* sector = nullptr;

        if (!DiscSector(uint32_t(at / kSector), sector))
            return false;

        const size_t offset = at % kSector, n = std::min<size_t>(count, kSector - offset);
        std::memcpy(out, sector + offset, n);
        at += n, out += n, count -= n;
    }

    return true;
}

bool RomReader::ReadFlat(uint64_t at, uint8_t* out, size_t count) {
    if (at > flatBytes || count > flatBytes - at)
        return Fail("Read beyond the game partition");

    const Partition& p = partitions[game];

    while (count) {
        const uint32_t sector = p.dataStart + uint32_t(at / kSectorData);
        const size_t offset = at % kSectorData;
        size_t n;

        if (wia) {
            Span span{};

            if (!WiaSectors(game, sector, span))
                return false;

            n = std::min(count, span.size - offset);

            if (span.data)
                std::memcpy(out, span.data + offset, n);
            else
                std::memset(out, 0, n);
        } else if (!offset && count >= kSectorData) {
            if (!IsoSectors(p, sector, out, count / kSectorData, n))
                return false;

            n *= kSectorData;
        } else {
            if (sector != dataSectorIndex) {
                dataSectorIndex = UINT32_MAX;
                dataSector.resize(kSectorData);
                size_t decoded = 0;

                if (!IsoSectors(p, sector, dataSector.data(), 1, decoded))
                    return false;

                dataSectorIndex = sector;
            }

            n = std::min(count, kSectorData - offset);
            std::memcpy(out, dataSector.data() + offset, n);
        }

        at += n, out += n, count -= n;
    }

    return true;
}

void CopyError(uint8_t* buffer, size_t capacity, const char* message) {
    if (!buffer || !capacity)
        return;

    const size_t n = std::min(std::strlen(message), capacity - 1);
    std::memcpy(buffer, message, n);
    buffer[n] = 0;
}
} // namespace

extern "C" void* mh3_rom_open(void* context, uint64_t size, MH3RomReadAt readAt, uint8_t* error, size_t capacity) {
    auto rom = std::make_unique<RomReader>(context, size, readAt);

    if (!rom->Open()) {
        CopyError(error, capacity, rom->error.c_str());
        return nullptr;
    }

    return rom.release();
}

extern "C" uint64_t mh3_rom_size(void* reader, int flat) {
    const auto* rom = static_cast<const RomReader*>(reader);
    return flat ? rom->FlatBytes() : rom->DiscBytes();
}

extern "C" int mh3_rom_read(void* reader, int flat, uint64_t offset, uint8_t* bytes, size_t count, uint8_t* error, size_t capacity) {
    auto* rom = static_cast<RomReader*>(reader);

    if (flat ? rom->ReadFlat(offset, bytes, count) : rom->ReadDisc(offset, bytes, count))
        return 0;

    CopyError(error, capacity, rom->error.c_str());
    return -1;
}

extern "C" void mh3_rom_close(void* reader) { delete static_cast<RomReader*>(reader); }
