#include "port_check.h"

#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif
#include "game_step.h"
#include "guest_call.h"
#include "gx_host.h"
#include "memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace PortCheck {

#ifdef MH3_DEVELOPMENT
namespace {
// MH3_PORT_CHECK (port_check.h, docs/flags.md): port names or `all`, fuzz
// entries (P:/E:/C:0xADDR) and the options.
const DevOptions& Options() {
    static const DevOptions options("MH3_PORT_CHECK", {"every", "originals", "dump", "fuzz-after", "fuzz-samples"}, DevOptions::Words::Allowed);
    return options;
}
} // namespace
#endif

bool SetSlow() {
#ifdef MH3_DEVELOPMENT
    const bool set = Options().Set();
#else
    const bool set = false;
#endif
    g_set.store(set ? 1 : 0, std::memory_order_relaxed);
    return set;
}

bool EnabledSlow(const char* name) {
#ifdef MH3_DEVELOPMENT
    const DevOptions& options = Options();
    return !options.List().empty() && (options.Listed("all") || options.Listed(name));
#else
    (void)name;
    return false;
#endif
}

bool OriginalsSlow() {
#ifdef MH3_DEVELOPMENT
    static const bool originals = Options().Has("originals");
    return originals;
#else
    return false;
#endif
}

#ifdef MH3_DEVELOPMENT
namespace {

struct Snapshot {
    std::vector<uint8_t> bytes;
    static uint8_t* Mem1() { return Memory::GetPointer(Memory::kMem1CachedBase, Memory::kMem1Size); }
    static uint8_t* Mem2() { return Memory::GetPointer(Memory::kMem2CachedBase, Memory::kMem2Size); }
    void Save() {
        bytes.resize(Memory::kMem1Size + Memory::kMem2Size);
        std::memcpy(bytes.data(), Mem1(), Memory::kMem1Size);
        std::memcpy(bytes.data() + Memory::kMem1Size, Mem2(), Memory::kMem2Size);
    }

    void Restore() const {
        std::memcpy(Mem1(), bytes.data(), Memory::kMem1Size);
        std::memcpy(Mem2(), bytes.data() + Memory::kMem1Size, Memory::kMem2Size);
    }

    static uint32_t Address(size_t offset) { return offset < Memory::kMem1Size ? Memory::kMem1CachedBase + uint32_t(offset) : Memory::kMem2CachedBase + uint32_t(offset - Memory::kMem1Size); }
};

// Fuzz: every call checked, whatever the clock or the rate.
bool forced = false;

uint32_t Word(const std::vector<uint8_t>& b, size_t i) {
    return uint32_t(b[i]) << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3];
}

} // namespace
#endif

void Run(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
#ifdef MH3_DEVELOPMENT
    static std::set<std::string> reported;
    static std::map<std::string, uint32_t> counts;
    static std::map<std::string, std::chrono::steady_clock::time_point> last;
    static std::chrono::steady_clock::time_point lastAny;
    uint32_t& checks = counts[name];

    // A check copies all of RAM four times: at most one every 500 ms, and per
    // port one every 2 s, so the game keeps most of its speed and every port
    // named gets its turn (Fuzz, and the `every` option, check every call).
    const auto now = std::chrono::steady_clock::now();
    auto& previous = last[name];
    static const bool every = Options().Has("every");

    if (GameStep::Ticks() != 1 || (!forced && !every && (now - lastAny < std::chrono::milliseconds(500) || now - previous < std::chrono::seconds(2)))) {
        port(ctx);
        return;
    }

    previous = lastAny = now;
    const CpuContext entry = *ctx;
    const GameStep::State clock = GameStep::Save();
    Snapshot before, expected;
    before.Save();

    // The GX FIFO bytes each writes are compared too; only the port's go on.
    std::vector<uint8_t> expectedGx, actualGx;
    CaptureGuestGx(&expectedGx);
    original(ctx);
    const CpuContext originalCtx = *ctx;
    expected.Save();
    before.Restore();
    GameStep::Restore(clock);
    *ctx = entry;
    CaptureGuestGx(&actualGx);
    port(ctx);
    CaptureGuestGx(nullptr);

    if (!actualGx.empty())
        SubmitGuestGxBytes(actualGx.data(), actualGx.size());

    Snapshot actual;
    actual.Save();
    const uint32_t sp = entry.gpr[1];
    size_t differences = 0;
    std::string lines;

    for (size_t i = 0; i + 4 <= actual.bytes.size(); i += 4) {
        if (i % 4096 == 0 && std::memcmp(&expected.bytes[i], &actual.bytes[i], 4096) == 0) {
            i += 4096 - 4;
            continue;
        }

        const uint32_t a = Word(expected.bytes, i), b = Word(actual.bytes, i);

        if (a == b)
            continue;

        const uint32_t address = Snapshot::Address(i);

        // Callee stack frames, and the LR save word the callee stores in the caller's.
        if (address < sp + 8 && address >= sp - 0x10000)
            continue;

        if (++differences <= 32) {
            char line[96];
            std::snprintf(line, sizeof line, "  %08x original %08x port %08x before %08x\n", address, a, b, Word(before.bytes, i));
            lines += line;
        }
    }

    if (ctx->gpr[1] != originalCtx.gpr[1] || ctx->gpr[2] != originalCtx.gpr[2] || ctx->gpr[13] != originalCtx.gpr[13])
        lines += "  stack/sda registers differ after return\n";

    if (expectedGx != actualGx) {
        size_t at = 0;

        while (at < expectedGx.size() && at < actualGx.size() && expectedGx[at] == actualGx[at])
            ++at;

        size_t bytes = 0;

        for (size_t i = 0; i < expectedGx.size() && i < actualGx.size(); ++i)
            bytes += expectedGx[i] != actualGx[i];

        char line[256];
        int n = std::snprintf(line, sizeof line, "  GX FIFO: %llu bytes original, %llu port, %llu differ from byte %llu:", static_cast<unsigned long long>(expectedGx.size()),
                              static_cast<unsigned long long>(actualGx.size()), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(at));
        for (size_t i = at & ~size_t(3); i < (at & ~size_t(3)) + 8 && n < 200; ++i)
            n += std::snprintf(line + n, sizeof line - n, " %02x/%02x", i < expectedGx.size() ? expectedGx[i] : 0, i < actualGx.size() ? actualGx[i] : 0);

        lines += line;
        lines += "\n";

        // dump=DIR: the first few differing streams, as NAME-N-{original,port}.gx.
        static const char* dump = Options().Value("dump");
        static uint32_t dumped = 0;

        if (dump && dumped < 8) {
            ++dumped;

            for (int which = 0; which < 2; ++which) {
                const std::string path = std::string(dump) + "/" + name + "-" + std::to_string(dumped) + (which ? "-port.gx" : "-original.gx");

                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    const std::vector<uint8_t>& b = which ? actualGx : expectedGx;
                    std::fwrite(b.data(), 1, b.size(), f);
                    std::fclose(f);
                }
            }
        }
    }

    ++checks;

    if (differences || !lines.empty()) {
        const std::string key = name + std::string(":") + lines.substr(0, lines.find('\n'));

        if (reported.insert(key).second)
            std::printf("MH3 port check: %s differs from the original (check %u, %llu words):\n%s", name, checks, static_cast<unsigned long long>(differences), lines.c_str());
    } else if ((checks & (checks - 1)) == 0) {
        std::printf("MH3 port check: %s matches the original (%u checks)\n", name, checks);
    }

    std::fflush(stdout);
#else
    (void)name;
    (void)original;
    port(ctx);
#endif
}

void RunPure(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*)) {
#ifdef MH3_DEVELOPMENT
    static std::map<std::string, uint64_t> counts;
    static std::set<std::string> reported;
    const CpuContext entry = *ctx;
    original(ctx);
    const uint64_t expected = returns == Returns::Gpr3 ? ctx->gpr[3] : ctx->fpr[1].raw;
    *ctx = entry;
    port(ctx);
    const uint64_t actual = returns == Returns::Gpr3 ? ctx->gpr[3] : ctx->fpr[1].raw;
    uint64_t& checks = counts[name];
    ++checks;

    if (expected != actual) {
        char line[256];
        std::snprintf(line, sizeof line, "MH3 port check: %s returns %016llx, the original %016llx (r3 %08x r4 %08x r5 %08x f1 %016llx f2 %016llx)\n", name, (unsigned long long)actual,
                      (unsigned long long)expected, entry.gpr[3], entry.gpr[4], entry.gpr[5], (unsigned long long)entry.fpr[1].raw, (unsigned long long)entry.fpr[2].raw);
        if (reported.size() < 64 && reported.insert(line).second)
            std::printf("%s", line);
    } else if ((checks & (checks - 1)) == 0) {
        std::printf("MH3 port check: %s matches the original (%llu calls)\n", name, (unsigned long long)checks);
    }

    std::fflush(stdout);
#else
    (void)name;
    (void)returns;
    (void)original;
    port(ctx);
#endif
}

namespace {
// returns: -1 none, else a Returns.
void CheckOutputs(CpuContext* ctx, const char* name, int returns, void (*original)(CpuContext*), void (*port)(CpuContext*), std::initializer_list<Region> outputs) {
#ifdef MH3_DEVELOPMENT
    static std::map<std::string, uint64_t> counts;
    static std::set<std::string> reported;
    std::vector<std::vector<uint8_t>> before, expected;

    for (const Region& r : outputs) {
        const uint8_t* out = Memory::GetPointer(r.address, r.bytes);
        before.emplace_back(out, out + r.bytes);
    }

    const CpuContext entry = *ctx;
    original(ctx);
    const auto result = [&] { return returns == int(Returns::Gpr3) ? uint64_t(ctx->gpr[3]) : ctx->fpr[1].raw; };
    const uint64_t expectedResult = result();
    size_t i = 0;

    for (const Region& r : outputs) {
        uint8_t* out = Memory::GetPointer(r.address, r.bytes);
        expected.emplace_back(out, out + r.bytes);
        std::memcpy(out, before[i++].data(), r.bytes);
    }

    *ctx = entry;
    port(ctx);
    uint64_t& checks = counts[name];
    ++checks;
    bool same = true;
    i = 0;

    if (returns >= 0 && result() != expectedResult) {
        same = false;
        char line[256];
        std::snprintf(line, sizeof line, "MH3 port check: %s returns %016llx, the original %016llx (r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x r8 %08x)\n", name,
                      (unsigned long long)result(), (unsigned long long)expectedResult, entry.gpr[3], entry.gpr[4], entry.gpr[5], entry.gpr[6], entry.gpr[7], entry.gpr[8]);
        if (reported.size() < 64 && reported.insert(line).second)
            std::printf("%s", line);
    }

    for (const Region& r : outputs) {
        const uint8_t* out = Memory::GetPointer(r.address, r.bytes);
        const std::vector<uint8_t>& want = expected[i++];
        size_t at = 0;

        while (at < r.bytes && out[at] == want[at])
            ++at;

        if (at == r.bytes)
            continue;

        same = false;
        char line[256];
        std::snprintf(line, sizeof line,
                      "MH3 port check: %s writes %02x at %08x, the original %02x (r3 %08x r4 %08x r5 %08x r6 %08x "
                      "r7 %08x r8 %08x)\n",
                      name, out[at], r.address + uint32_t(at), want[at], entry.gpr[3], entry.gpr[4], entry.gpr[5], entry.gpr[6], entry.gpr[7], entry.gpr[8]);
        if (reported.size() < 64 && reported.insert(line).second)
            std::printf("%s", line);

        break;
    }

    if (same && (checks & (checks - 1)) == 0)
        std::printf("MH3 port check: %s matches the original (%llu calls)\n", name, (unsigned long long)checks);

    std::fflush(stdout);
#else
    (void)name;
    (void)returns;
    (void)original;
    (void)outputs;
    port(ctx);
#endif
}
} // namespace

void RunOutput(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*), std::initializer_list<Region> outputs) {
    CheckOutputs(ctx, name, -1, original, port, outputs);
}

void RunOutput(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*), void (*port)(CpuContext*), std::initializer_list<Region> outputs) {
    CheckOutputs(ctx, name, int(returns), original, port, outputs);
}

void FuzzSlow(CpuContext* ctx, char kind, uint32_t work) {
#ifdef MH3_DEVELOPMENT
    // The fuzz entries: KIND:0xADDR.
    static const std::vector<std::string> fuzz = [] {
        std::vector<std::string> entries;

        for (const std::string& item : Options().List())
            if (item.size() > 2 && item[1] == ':')
                entries.push_back(item);
        return entries;
    }();
    static const uint32_t after = uint32_t(Options().Number("fuzz-after", 900));
    static std::map<char, uint32_t> calls;

    if (fuzz.empty() || ++calls[kind] != after)
        return;

    constexpr uint32_t kStepCounter = 0x8065AA6C;
    const CpuContext entry = *ctx;
    const GameStep::State clock = GameStep::Save();
    Snapshot start;
    start.Save();
    GuestCall game{ctx};
    uint32_t runs = 0;
    forced = true;

    for (const std::string& item : fuzz) {
        if (item[0] != kind)
            continue;

        const uint32_t fn = uint32_t(std::strtoul(item.c_str() + 2, nullptr, 16));

        if (kind == 'P') {
            // Samples (fuzz-samples, default 64) of the action state
            // (each in turn), the step counter and countdown, the args r4 = r5
            // (0-3) and the inputs: the command bit words (+0xE0..+0xFF), the
            // pad (+0xC8, +0xCC) and the flag bytes +6, +7.
            static const uint32_t samples = uint32_t(Options().Number("fuzz-samples", 64));
            constexpr uint32_t kTimers[] = {0, 1, 2, 40, 400};
            constexpr uint32_t kBits[] = {0, ~0u, 0x55555555, 0xAAAAAAAA};

            for (uint32_t i = 0; i < samples; ++i) {
                uint32_t r = fn ^ (i * 0x9E3779B9u);
                r ^= r >> 15;
                r *= 0x2C1B3C6Du;
                r ^= r >> 12;
                const uint32_t state = i % 8, timer = kTimers[r % 5], bits = kBits[(r >> 8) % 4], arg = (r >> 16) % 4;
                Memory::Write8(work + 5, uint8_t(state));
                Memory::Write32(work + 0x20, state * 25);
                Memory::Write32(work + 0x28, timer);

                for (uint32_t k = 0xE0; k < 0x100; k += 4)
                    Memory::Write32(work + k, bits);

                Memory::Write16(work + 0xC8, uint16_t(bits));
                Memory::Write16(work + 0xCC, uint16_t(bits >> 1));
                Memory::Write8(work + 6, uint8_t(bits & 1));
                Memory::Write8(work + 7, uint8_t(bits >> 31));
                game(fn, {work, arg, arg});
                start.Restore();
                GameStep::Restore(clock);
                *ctx = entry;
                ++runs;
            }

            std::printf("MH3 port fuzz: %08x done\n", fn);
            std::fflush(stdout);
            continue;
        }

        for (uint32_t arg = 0; arg < 2; ++arg) {
            const uint32_t counter = Memory::Read32(kStepCounter);

            for (uint32_t phase = 0; phase < 64; phase += 3) {
                Memory::Write32(kStepCounter, (counter & ~63u) + 64 + phase);
                game(fn, {work, arg, arg});
                start.Restore();
                GameStep::Restore(clock);
                *ctx = entry;
                ++runs;
            }
        }
    }

    forced = false;

    // C: entries (with the player's hook): started once and kept, so the
    // checks see the updates of what they start.
    for (const std::string& item : fuzz) {
        if (kind != 'P' || item[0] != 'C')
            continue;

        const uint32_t fn = uint32_t(std::strtoul(item.c_str() + 2, nullptr, 16));
        const uint32_t zero = (entry.gpr[1] - 0x400) & ~15u; // below the stack
        for (uint32_t i = 0; i < 16; i += 4)
            Memory::Write32(zero + i, 0);

        game(fn, {work + 0x3C, zero, 0, work}); // PLW+0x3C: the position
        *ctx = entry;
        std::printf("MH3 port fuzz: started %08x\n", fn);
    }

    std::printf("MH3 port fuzz: %c work %08x, %u calls\n", kind, work, runs);
    std::fflush(stdout);
#else
    (void)ctx;
    (void)kind;
    (void)work;
#endif
}

} // namespace PortCheck
