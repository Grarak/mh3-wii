#pragma once
#include "isa/ppc_isa_thread_state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct CpuContext;

namespace RecompMod {

using InitializerFn = void (*)();

struct MemoryReservation {
    uint32_t start = 0;
    uint32_t end = 0;
    std::string name;
};

// Diagnostic-only, read by fatal reporters (unmapped access, executable-write, CPU dumps).
// Each thread writes its own record (PpcThread::Of the dispatched context), so
// the game thread and the guest worker never share the written cache line.

class ScopedTranslatedExecutionAddress {
  public:
    ScopedTranslatedExecutionAddress(uint32_t address, const CpuContext* cpu) noexcept : thread_(PpcThread::Of(cpu)), previous_(thread_.executionAddress) {
        // Address 0 means "nothing new to report" - the enclosing scope's value
        // stays visible, and the unconditional restore below keeps nesting exact.
        if (address != 0) {
            thread_.executionAddress = address;
        }
    }

    ~ScopedTranslatedExecutionAddress() noexcept { thread_.executionAddress = previous_; }

    ScopedTranslatedExecutionAddress(const ScopedTranslatedExecutionAddress&) = delete;
    ScopedTranslatedExecutionAddress& operator=(const ScopedTranslatedExecutionAddress&) = delete;

  private:
    PpcThread::State& thread_;
    uint32_t previous_ = 0;
};

// Two initializer phases, both emitted by the translator's mod data-patch
// writer. (A third, plain RegisterInitializer/RunInitializers pair existed with
// no registrant on either side and was removed.)
void RegisterMemoryInitializer(InitializerFn fn);
void RunMemoryInitializers();
void RegisterPostRelInitializer(InitializerFn fn);
void RunPostRelInitializers();

// The generated call site passes a UTF-8 literal; it is decoded once here and
// stays a path from then on.
void RegisterDvdOverlayRoot(std::string root);
const std::vector<std::filesystem::path>& DvdOverlayRoots();

// Riivolution settings pinned by the distribution's recomp.yml. The XML path is
// relative to the pack/overlay root; option selections use Riivolution's 1-based
// choice index (0 disables the option).
struct RiivolutionOptionSelection {
    std::string section;
    std::string option;
    uint32_t choice = 0;
};

void RegisterRiivolutionXml(const char* packRelativePath);
void RegisterRiivolutionOption(const char* sectionName, const char* optionName, unsigned int choice);
const std::string& RiivolutionXml();
const std::vector<RiivolutionOptionSelection>& RiivolutionOptionSelections();

void RegisterMemoryReservation(uint32_t start, uint32_t end, std::string name);
const std::vector<MemoryReservation>& MemoryReservations();

uint32_t CurrentTranslatedExecutionAddress() noexcept;

void RegisterExecutableRange(uint32_t start, uint32_t end, std::string name);
void UnregisterExecutableRange(uint32_t start, uint32_t end);

// Fixed-address RSO code is callable only while its verified image is linked.
void CheckRsoExecution(uint32_t address);
bool TryDispatchRsoOverlay(uint32_t address, CpuContext* cpu);

// Whether the RSO module `name` (e.g. "net_data.rso") is linked now.
bool RsoLinked(std::string_view name);

// Host services that write guest memory (DMA, report copies) check here that
// they leave translated code untouched.
void CheckExecutableWrite(uint32_t address, size_t length, uint64_t value);

// Development builds: guest stores go straight to memory, so a store that
// changes translated code is caught afterwards by comparing each registered
// range against the checksum taken on its first verification.
void VerifyExecutableRanges();

} // namespace RecompMod
