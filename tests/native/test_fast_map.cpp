// FastMap (runtime/include/fast_map.h) against std::unordered_map: random
// inserts, assignments, lookups and erases (also while iterating), copies
// and moves; value addresses stay put until erased.
#include "fast_map.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <unordered_map>

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::fprintf(stderr, "FAILED %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                \
        }                                                              \
    } while (0)

template <typename K>
void Run(uint64_t seed, K range) {
    std::mt19937_64 rng(seed);
    FastMap<K, uint64_t> map;
    std::unordered_map<K, uint64_t> reference;
    std::unordered_map<K, const uint64_t*> addresses;
    for (int step = 0; step < 200000; ++step) {
        const K key = K(rng() % range) * K(16) + K(0x80000000u);
        switch (rng() % 7) {
        case 0:
        case 1: {
            const uint64_t value = rng();
            auto [it, inserted] = map.insert_or_assign(key, value);
            const bool fresh = reference.insert_or_assign(key, value).second;
            CHECK(inserted == fresh && it->second == value);
            if (inserted)
                addresses[key] = &it->second;
            break;
        }
        case 2: {
            auto [it, inserted] = map.try_emplace(key, 7);
            const bool fresh = reference.try_emplace(key, 7).second;
            CHECK(inserted == fresh);
            if (inserted)
                addresses[key] = &it->second;
            break;
        }
        case 3: {
            const auto it = map.find(key);
            const auto ref = reference.find(key);
            CHECK((it == map.end()) == (ref == reference.end()));
            if (ref != reference.end()) {
                CHECK(it->second == ref->second);
                CHECK(&it->second == addresses[key]);
            }
            break;
        }
        case 4:
            CHECK(map.erase(key) == reference.erase(key));
            addresses.erase(key);
            break;
        case 5:
            if (rng() % 64 == 0) {
                for (auto it = map.begin(); it != map.end();) {
                    if (it->first % 3 == 0) {
                        reference.erase(it->first);
                        addresses.erase(it->first);
                        it = map.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            break;
        case 6:
            if (rng() % 4096 == 0) {
                FastMap<K, uint64_t> copy = map;
                CHECK(copy.size() == map.size());
                for (const auto& [k, v] : map)
                    CHECK(copy.at(k) == v);
                map = std::move(copy);
                for (auto& [k, v] : map)
                    addresses[k] = &v;
            }
            break;
        }
        CHECK(map.size() == reference.size());
    }
    size_t seen = 0;
    for (const auto& [k, v] : map) {
        CHECK(reference.at(k) == v);
        ++seen;
    }
    CHECK(seen == reference.size());
    map.clear();
    CHECK(map.empty() && map.begin() == map.end());
}

int main() {
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        Run<uint32_t>(seed, 64);
        Run<uint32_t>(seed, 5000);
        Run<uint64_t>(seed, 1u << 20);
    }
    if (failures)
        return 1;
    std::printf("FastMap check passed\n");
    return 0;
}
