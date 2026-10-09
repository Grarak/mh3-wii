// FastMap and FastSet (aurora-main/include/aurora/fast_map.h) against the
// standard containers: random inserts, assignments, lookups and erases (also
// while iterating and through erase_if), copies and moves, with integer and
// struct keys; value addresses stay put until erased.
#include <aurora/fast_map.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <unordered_set>

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::fprintf(stderr, "FAILED %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                \
        }                                                              \
    } while (0)

// A struct key as Aurora's caches use them: a pointer, split integers, a
// float and an enum, hashed through FastHashValue.
struct StructKey {
    enum class Kind : uint8_t { Even, Odd };
    const void* data = nullptr;
    uint32_t low = 0;
    uint16_t high = 0;
    float scale = 0.f;
    Kind kind = Kind::Even;
    bool operator==(const StructKey&) const = default;
    friend uint32_t FastHashValue(const StructKey& key) { return FastHash(key.data, key.low, key.high, key.scale, key.kind); }
};

// Every key is built from an integer id, which the reference containers use.
template <typename K>
K MakeKey(uint64_t id) {
    if constexpr (std::is_same_v<K, StructKey>) {
        return {reinterpret_cast<const void*>(uintptr_t(0x10000 + (id & 0xFF) * 32)), uint32_t(id >> 8), uint16_t(id >> 40),
                float(id % 5) * 0.5f, id & 1 ? StructKey::Kind::Odd : StructKey::Kind::Even};
    } else {
        return K(id) * K(16) + K(0x80000000u);
    }
}

template <typename K>
uint64_t KeyId(const K& key) {
    if constexpr (std::is_same_v<K, StructKey>)
        return (uint64_t(key.high) << 40) | (uint64_t(key.low) << 8) | ((reinterpret_cast<uintptr_t>(key.data) - 0x10000) / 32);
    else
        return uint64_t((key - K(0x80000000u)) / K(16));
}

template <typename K>
void Run(uint64_t seed, uint64_t range) {
    std::mt19937_64 rng(seed);
    FastMap<K, uint64_t> map;
    std::unordered_map<uint64_t, uint64_t> reference;
    std::unordered_map<uint64_t, const uint64_t*> addresses;

    for (int step = 0; step < 200000; ++step) {
        const uint64_t id = rng() % range;
        const K key = MakeKey<K>(id);
        CHECK(KeyId(key) == id);

        switch (rng() % 8) {
        case 0:
        case 1: {
            const uint64_t value = rng();
            auto [it, inserted] = map.insert_or_assign(key, value);
            const bool fresh = reference.insert_or_assign(id, value).second;
            CHECK(inserted == fresh && it->second == value);

            if (inserted)
                addresses[id] = &it->second;

            break;
        }
        case 2: {
            auto [it, inserted] = map.try_emplace(key, 7);
            const bool fresh = reference.try_emplace(id, 7).second;
            CHECK(inserted == fresh);

            if (inserted)
                addresses[id] = &it->second;

            break;
        }
        case 3: {
            const auto it = map.find(key);
            const auto ref = reference.find(id);
            CHECK((it == map.end()) == (ref == reference.end()));

            if (ref != reference.end()) {
                CHECK(it->second == ref->second);
                CHECK(&it->second == addresses[id]);
            }

            break;
        }
        case 4:
            CHECK(map.erase(key) == reference.erase(id));
            addresses.erase(id);
            break;
        case 5:
            if (rng() % 64 == 0) {
                for (auto it = map.begin(); it != map.end();) {
                    if (KeyId(it->first) % 3 == 0) {
                        reference.erase(KeyId(it->first));
                        addresses.erase(KeyId(it->first));
                        it = map.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            break;
        case 6:
            if (rng() % 64 == 0) {
                const uint64_t bit = rng() & 1;
                size_t expected = 0;

                for (auto it = reference.begin(); it != reference.end();) {
                    if ((it->second & 1) == bit) {
                        addresses.erase(it->first);
                        it = reference.erase(it);
                        ++expected;
                    } else {
                        ++it;
                    }
                }

                CHECK(map.erase_if([bit](const auto& entry) { return (entry.second & 1) == bit; }) == expected);
            }

            break;
        case 7:
            if (rng() % 4096 == 0) {
                FastMap<K, uint64_t> copy = map;
                CHECK(copy.size() == map.size());

                for (const auto& [k, v] : map)
                    CHECK(copy.at(k) == v);

                map = std::move(copy);

                for (auto& [k, v] : map)
                    addresses[KeyId(k)] = &v;
            }

            break;
        }

        CHECK(map.size() == reference.size());
    }

    size_t seen = 0;

    for (const auto& [k, v] : map) {
        CHECK(reference.at(KeyId(k)) == v);
        ++seen;
    }

    CHECK(seen == reference.size());
    map.clear();
    CHECK(map.empty() && map.begin() == map.end());
}

template <typename K>
void RunSet(uint64_t seed, uint64_t range) {
    std::mt19937_64 rng(seed);
    FastSet<K> set;
    std::unordered_set<uint64_t> reference;

    for (int step = 0; step < 100000; ++step) {
        const uint64_t id = rng() % range;
        const K key = MakeKey<K>(id);

        switch (rng() % 5) {
        case 0:
        case 1:
            CHECK(set.insert(key).second == reference.insert(id).second);
            break;
        case 2:
            CHECK(set.contains(key) == reference.contains(id));
            break;
        case 3:
            CHECK(set.erase(key) == reference.erase(id));
            break;
        case 4:
            if (rng() % 64 == 0) {
                for (auto it = set.begin(); it != set.end();) {
                    if (KeyId(*it) % 3 == 0) {
                        reference.erase(KeyId(*it));
                        it = set.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            break;
        }

        CHECK(set.size() == reference.size());
    }

    size_t seen = 0;

    for (const K& key : set) {
        CHECK(reference.contains(KeyId(key)));
        ++seen;
    }

    CHECK(seen == reference.size());
}

// Direct-mapped caches index with the hash's low bits: consecutive 32-byte
// apart pointers must spread over them like random values (about 162 of 256
// buckets used).
void CheckLowBits() {
    bool used[256] = {};
    int distinct = 0;

    for (uintptr_t i = 0; i < 256; ++i) {
        const uint32_t bucket = FastHash(reinterpret_cast<const void*>(0x81230000 + i * 32), 64u, 64u) & 255;
        distinct += !used[bucket];
        used[bucket] = true;
    }

    CHECK(distinct > 140);
}

int main() {
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        Run<uint32_t>(seed, 64);
        Run<uint32_t>(seed, 5000);
        Run<uint64_t>(seed, 1u << 20);
        Run<StructKey>(seed, 5000);
        Run<StructKey>(seed, uint64_t(1) << 44);
        RunSet<uint32_t>(seed, 3000);
        RunSet<StructKey>(seed, 3000);
    }

    CheckLowBits();

    if (failures)
        return 1;

    std::printf("FastMap check passed\n");
    return 0;
}
