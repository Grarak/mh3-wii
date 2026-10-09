#include "port_check.h"

#include "audio/ax_dsp.h"
#include "game_step.h"
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

bool Enabled(const char* name) {
#ifdef MH3_DEVELOPMENT
    static const std::string list = [] {
        const char* v = std::getenv("MH3_PORT_CHECK");
        return std::string(v ? v : "");
    }();
    if (list.empty()) return false;
    if (list == "all") return true;
    const std::string n = name;
    for (size_t at = 0; (at = list.find(n, at)) != std::string::npos; at += n.size())
        if ((at == 0 || list[at - 1] == ',') && (at + n.size() == list.size() || list[at + n.size()] == ','))
            return true;
    return false;
#else
    (void)name;
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
    static uint32_t Address(size_t offset) {
        return offset < Memory::kMem1Size ? Memory::kMem1CachedBase + uint32_t(offset)
                                          : Memory::kMem2CachedBase + uint32_t(offset - Memory::kMem1Size);
    }
};

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
    // named gets its turn.
    const auto now = std::chrono::steady_clock::now();
    auto& previous = last[name];
    if (GameStep::Ticks() != 1 || now - lastAny < std::chrono::milliseconds(500) ||
        now - previous < std::chrono::seconds(2)) {
        port(ctx);
        return;
    }
    previous = lastAny = now;
    // The audio mix worker writes voice state into guest RAM: let it finish so
    // the restore below does not roll its writes back (guest code starts mixes).
    AxDspHle::JoinMixWorker();
    const CpuContext entry = *ctx;
    const GameStep::State clock = GameStep::Save();
    Snapshot before, expected;
    before.Save();
    original(ctx);
    const CpuContext originalCtx = *ctx;
    expected.Save();
    before.Restore();
    GameStep::Restore(clock);
    *ctx = entry;
    port(ctx);

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
        if (a == b) continue;
        const uint32_t address = Snapshot::Address(i);
        // Callee stack frames, and the LR save word the callee stores in the caller's.
        if (address < sp + 8 && address >= sp - 0x10000) continue;
        if (++differences <= 32) {
            char line[96];
            std::snprintf(line, sizeof line, "  %08x original %08x port %08x before %08x\n", address, a, b,
                          Word(before.bytes, i));
            lines += line;
        }
    }
    if (ctx->gpr[1] != originalCtx.gpr[1] || ctx->gpr[2] != originalCtx.gpr[2] || ctx->gpr[13] != originalCtx.gpr[13])
        lines += "  stack/sda registers differ after return\n";
    ++checks;
    if (differences || !lines.empty()) {
        const std::string key = name + std::string(":") + lines.substr(0, lines.find('\n'));
        if (reported.insert(key).second)
            std::printf("MH3 port check: %s differs from the original (check %u, %zu words):\n%s", name, checks,
                        differences, lines.c_str());
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

void RunPure(CpuContext* ctx, const char* name, Returns returns, void (*original)(CpuContext*),
             void (*port)(CpuContext*)) {
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
        std::snprintf(line, sizeof line,
                      "MH3 port check: %s returns %016llx, the original %016llx (r3 %08x r4 %08x r5 %08x f1 %016llx f2 %016llx)\n",
                      name, (unsigned long long)actual, (unsigned long long)expected, entry.gpr[3], entry.gpr[4],
                      entry.gpr[5], (unsigned long long)entry.fpr[1].raw, (unsigned long long)entry.fpr[2].raw);
        if (reported.size() < 64 && reported.insert(line).second) std::printf("%s", line);
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

void RunOutput(CpuContext* ctx, const char* name, void (*original)(CpuContext*), void (*port)(CpuContext*),
               std::initializer_list<Region> outputs) {
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
    for (const Region& r : outputs) {
        const uint8_t* out = Memory::GetPointer(r.address, r.bytes);
        const std::vector<uint8_t>& want = expected[i++];
        size_t at = 0;
        while (at < r.bytes && out[at] == want[at]) ++at;
        if (at == r.bytes) continue;
        same = false;
        char line[256];
        std::snprintf(line, sizeof line,
                      "MH3 port check: %s writes %02x at %08x, the original %02x (r3 %08x r4 %08x r5 %08x r6 %08x "
                      "r7 %08x r8 %08x)\n",
                      name, out[at], r.address + uint32_t(at), want[at], entry.gpr[3], entry.gpr[4], entry.gpr[5],
                      entry.gpr[6], entry.gpr[7], entry.gpr[8]);
        if (reported.size() < 64 && reported.insert(line).second) std::printf("%s", line);
        break;
    }
    if (same && (checks & (checks - 1)) == 0)
        std::printf("MH3 port check: %s matches the original (%llu calls)\n", name, (unsigned long long)checks);
    std::fflush(stdout);
#else
    (void)name;
    (void)original;
    (void)outputs;
    port(ctx);
#endif
}

} // namespace PortCheck
