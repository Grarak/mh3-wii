#include "native_frame_statistics.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::fprintf(stderr, "Native statistics check failed at line %d: %s\n", __LINE__, #expression); \
        std::exit(1); \
    } \
} while (false)

using namespace mh3::statistics;

int main() {
    NativeFrameAccumulator stats;
    const OwnerCpuSample initial{500, 7, 2, 0x40000, true};
    stats.Reset(1000, initial);
    stats.Begin(2000);
    stats.End(7000, true);
    // The gap represents pacing/preemption between frames and must not enter
    // frame wall time. A skipped frame still executes logical game work.
    stats.Begin(17000);
    stats.End(19000, false);
    const auto first = stats.Take(21000, {507, 7, 2, 0x40000, true});
    CHECK(first.window_ns == 20000);
    CHECK(first.logical_frames == 2 && first.rendered_frames == 1 && first.skipped_frames == 1);
    CHECK(first.measured_frames == 2 && first.frame_wall_ns == 7000 && first.frame_wall_max_ns == 5000);
    CHECK(first.rendered_measured_frames == 1 && first.rendered_wall_ns == 5000 && first.rendered_wall_max_ns == 5000);
    CHECK(first.owner_running_available && first.owner_running_us == 7 && first.owner_core == 2);

    // Counter windows reset, while an active frame survives a report boundary.
    stats.Begin(22000);
    const auto empty = stats.Take(23000, {508, 7, 2, 0x40000, true});
    CHECK(empty.logical_frames == 0 && empty.frame_wall_ns == 0);
    stats.End(25000, true);
    const auto crossing = stats.Take(27000, {510, 7, 2, 0x40000, true});
    CHECK(crossing.logical_frames == 1 && crossing.frame_wall_ns == 3000);
    CHECK(crossing.owner_running_available && crossing.owner_running_us == 2);

    // A failed kernel query, changed owner or regressing clock cannot invent
    // scheduled execution time or include stale samples in a later window.
    CHECK(!stats.Take(29000, {}).owner_running_available);
    CHECK(!stats.Take(31000, {513, 7, 2, 0x40000, true}).owner_running_available);
    CHECK(!stats.Take(33000, {100, 8, 2, 0x40000, true}).owner_running_available);
    CHECK(!stats.Take(35000, {99, 8, 2, 0x40000, true}).owner_running_available);
    CHECK(stats.Take(37000, {101, 8, 2, 0x40000, true}).owner_running_us == 2);

    // Missing/reversed timing brackets retain the completed-frame count but
    // never contribute bogus durations to the measured-frame denominator.
    stats.End(38000, false);
    stats.Begin(41000);
    stats.End(40000, true);
    const auto invalid = stats.Take(43000, {});
    CHECK(invalid.logical_frames == 2 && invalid.measured_frames == 0 && invalid.frame_wall_ns == 0);
    std::puts("Native frame statistics: logical/rendered/skipped, frame wall and owner CPU scope PASS");
}
