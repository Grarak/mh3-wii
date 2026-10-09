#include "village_boot.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
enum class State { Off, Navigating, Complete, Cancelled, TimedOut };
struct Navigator {
    State state = State::Off;
    ClassicControllerSample Update(ClassicControllerSample input, uint64_t milliseconds) {
        if (state != State::Navigating) return input;
        if (input.buttons || std::abs(int(input.leftX)) > 128 || std::abs(int(input.leftY)) > 128 ||
            std::abs(int(input.rightX)) > 128 || std::abs(int(input.rightY)) > 128) {
            state = State::Cancelled;
            return input;
        }
        if (milliseconds >= 180000) {
            state = State::TimedOut;
            return input;
        }
        // Repeated edges tolerate menu animations, movie startup and slow
        // shader compilation. Never continue these pulses into the village.
        return {true, uint16_t(milliseconds % 1500 < 250 ? 0x10 : 0)};
    }
};
Navigator navigator;
Clock::time_point started;
uint32_t Word(std::span<const uint8_t> data, size_t offset) {
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
}
bool HasFirstCharacter(std::span<const uint8_t> data) {
    // Original readDataFile validation (0x80441B9C) sums bytes 8..0x13FFF.
    // Character records are 0x6000 bytes, starting at 0x48 (0x80441DB0).
    return data.size() == 0x14000 && (Word(data, 0) & 1) && Word(data, 0x48) != 0 &&
           std::accumulate(data.begin() + 8, data.end(), uint32_t(0)) == Word(data, 4);
}
}

void StartVillageBoot() {
    const char* override = std::getenv("MH3_USER_DIR");
    const auto path = std::filesystem::path(override ? override : "saves") /
        "nand/title/00010000/524d4850/data/nocopy/data00";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != 0x14000)
        throw std::runtime_error("--village requires an existing File 1 save: " + path.string());
    std::vector<uint8_t> data(0x14000);
    file.seekg(0); file.read(reinterpret_cast<char*>(data.data()), data.size());
    if (!file || !HasFirstCharacter(data))
        throw std::runtime_error("--village requires a valid File 1 with its first character populated; create one through normal startup first");
    navigator.state = State::Navigating;
    started = Clock::now();
    std::cerr << "MH3 village shortcut: entering File 1 / character 1; press a controller button to cancel\n";
}

void FinishVillageBoot() {
    if (navigator.state != State::Navigating) return;
    navigator.state = State::Complete;
    std::cerr << "MH3 village shortcut: village module verified; normal controls restored\n";
}

ClassicControllerSample ReadGameController() {
    const auto before = navigator.state;
    const auto input = ReadHostController();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    const auto output = navigator.Update(input, uint64_t(std::max<int64_t>(elapsed, 0)));
    if (before != navigator.state) {
        if (navigator.state == State::Cancelled) std::cerr << "MH3 village shortcut: cancelled by input; normal controls restored\n";
        if (navigator.state == State::TimedOut) std::cerr << "MH3 village shortcut: timed out; use normal controls to continue\n";
    }
    return output;
}

void RunVillageBootSelfTests() {
    auto check = [](bool valid) { if (!valid) throw std::runtime_error("Village shortcut regression failed"); };
    Navigator test{State::Navigating};
    check(test.Update({}, 0).buttons == 0x10);
    check(test.Update({}, 250).buttons == 0);
    check(test.Update({}, 1500).buttons == 0x10);
    test.state = State::Complete;
    check(test.Update({}, 3000).buttons == 0);
    test.state = State::Navigating;
    check(test.Update({true, 0x40}, 0).buttons == 0x40 && test.state == State::Cancelled);
    check(test.Update({}, 1500).buttons == 0);
    test.state = State::Navigating;
    check(test.Update({}, 180000).buttons == 0 && test.state == State::TimedOut);
    std::vector<uint8_t> data(0x14000);
    check(!HasFirstCharacter(data));
    data[3] = 1; data[0x4B] = '1'; data[7] = '1';
    check(HasFirstCharacter(data));
    ++data.back(); check(!HasFirstCharacter(data));
    data.resize(16); check(!HasFirstCharacter(data));
}
