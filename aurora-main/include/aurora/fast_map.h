#pragma once

// Hash containers with power-of-two buckets: a lookup masks the hash.
// libstdc++'s std::unordered_map takes the hash modulo a prime bucket count,
// which on the Cortex-A9 (no divide instruction) is a __udivmodsi4 call per
// lookup; the model draw path's lookups made that 3.3% of the Vita game
// thread. The runtime and Aurora share this one implementation.
//
// FastMap is a drop-in for the std::unordered_map subset both use. Entries
// are heap nodes that never move, so references and pointers to values stay
// valid until their entry is erased, as with std::unordered_map. Open
// addressing with linear probing over node pointers; erase leaves a
// tombstone, so erasing (also while iterating, `it = map.erase(it)` or
// `map.erase(it++)`) moves no other entry. Inserts may rehash, which
// invalidates iterators (not references), also as with std::unordered_map.
//
// Keys: integers, enums and pointers hash as themselves. A struct key
// defines `friend uint32_t FastHashValue(const Key&)` (found by argument-
// dependent lookup), normally `return FastHash(key.a, key.b, ...);`, or the
// map takes a hasher type as its third parameter. Keys compare with ==.
// FastSet is the set form.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// Order-dependent hash of a struct key's fields: 32-bit multiply-rotate steps
// (FxHash) and murmur3's finalizer, so every field reaches both the low bits
// (a direct-mapped cache's index) and the high bits (a FastMap bucket). A
// 64-bit field mixes in both halves; floats hash their bits, with -0 as 0.
class FastHasher {
  public:
    template <typename T>
    FastHasher& add(T value) {
        if constexpr (std::is_pointer_v<T>) {
            return add(reinterpret_cast<uintptr_t>(value));
        } else if constexpr (std::is_enum_v<T>) {
            return add(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_floating_point_v<T>) {
            static_assert(sizeof(T) == 4, "FastHasher hashes 32-bit floats");
            return add(value == 0 ? 0u : std::bit_cast<uint32_t>(value));
        } else {
            static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "FastHasher hashes integers, enums, pointers and floats");
            if constexpr (sizeof(T) > 4)
                Mix(uint32_t(uint64_t(value) >> 32));
            Mix(uint32_t(value));
            return *this;
        }
    }
    uint32_t finish() const {
        uint32_t h = h_;
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        return h ^ (h >> 16);
    }

  private:
    void Mix(uint32_t v) { h_ = (std::rotl(h_, 5) ^ v) * 0x9E3779B9u; }
    uint32_t h_ = 0;
};

template <typename... T>
uint32_t FastHash(const T&... fields) {
    FastHasher hasher;
    (hasher.add(fields), ...);
    return hasher.finish();
}

// The default hasher: integers, enums and pointers as themselves (FastMap
// mixes them), anything else through its FastHashValue.
template <typename K>
struct FastMapHash {
    uint64_t operator()(const K& key) const {
        if constexpr (std::is_pointer_v<K>)
            return uint64_t(reinterpret_cast<uintptr_t>(key));
        else if constexpr (std::is_integral_v<K> || std::is_enum_v<K>)
            return uint64_t(key);
        else
            return uint64_t(FastHashValue(key));
    }
};

template <typename K, typename V, typename H = FastMapHash<K>>
class FastMap {
  public:
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;
    using size_type = size_t;
    using hasher = H;

  private:
    static value_type* Tomb() { return reinterpret_cast<value_type*>(uintptr_t(1)); }
    static bool Live(const value_type* p) { return p && p != Tomb(); }

    template <bool Const>
    class Iter {
        using Map = std::conditional_t<Const, const FastMap, FastMap>;
        Map* map_ = nullptr;
        uint32_t i_ = 0;
        void Skip() {
            while (i_ < map_->slots_.size() && !Live(map_->slots_[i_]))
                ++i_;
        }

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::conditional_t<Const, const FastMap::value_type, FastMap::value_type>;
        using difference_type = ptrdiff_t;
        using pointer = value_type*;
        using reference = value_type&;
        Iter() = default;
        Iter(Map* map, uint32_t i, bool skip) : map_(map), i_(i) {
            if (skip)
                Skip();
        }
        template <bool C = Const, typename = std::enable_if_t<C>>
        Iter(const Iter<false>& other) : map_(other.map_), i_(other.i_) {}
        reference operator*() const { return *map_->slots_[i_]; }
        pointer operator->() const { return map_->slots_[i_]; }
        Iter& operator++() {
            ++i_;
            Skip();
            return *this;
        }
        Iter operator++(int) {
            Iter old = *this;
            ++*this;
            return old;
        }
        friend bool operator==(const Iter& a, const Iter& b) { return a.i_ == b.i_; }
        friend bool operator!=(const Iter& a, const Iter& b) { return a.i_ != b.i_; }
        friend class FastMap;
        friend class Iter<!Const>;
    };

  public:
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    FastMap() = default;
    FastMap(const FastMap& other) { *this = other; }
    FastMap(FastMap&& other) noexcept { Swap(other); }
    FastMap& operator=(const FastMap& other) {
        if (this != &other) {
            clear();
            reserve(other.size_);
            for (const auto& item : other)
                try_emplace(item.first, item.second);
        }
        return *this;
    }
    FastMap& operator=(FastMap&& other) noexcept {
        if (this != &other) {
            clear();
            slots_.clear();
            slots_.shrink_to_fit();
            used_ = 0;
            shift_ = 32;
            Swap(other);
        }
        return *this;
    }
    ~FastMap() { clear(); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    iterator begin() { return iterator(this, 0, true); }
    iterator end() { return iterator(this, uint32_t(slots_.size()), false); }
    const_iterator begin() const { return const_iterator(this, 0, true); }
    const_iterator end() const { return const_iterator(this, uint32_t(slots_.size()), false); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    void clear() {
        for (value_type*& slot : slots_) {
            if (Live(slot))
                delete slot;
            slot = nullptr;
        }
        size_ = used_ = 0;
    }

    // Room for `n` entries without a rehash.
    void reserve(size_t n) {
        size_t capacity = 8;
        while (capacity < n * 2)
            capacity *= 2;
        if (capacity > slots_.size())
            Rehash(capacity);
    }

    iterator find(const K& key) { return iterator(this, Find(key), false); }
    const_iterator find(const K& key) const { return const_iterator(this, Find(key), false); }
    bool contains(const K& key) const { return Find(key) != slots_.size(); }
    size_t count(const K& key) const { return contains(key) ? 1 : 0; }

    V& at(const K& key) {
        const uint32_t i = Find(key);
        if (i == slots_.size())
            throw std::out_of_range("FastMap::at");
        return slots_[i]->second;
    }
    const V& at(const K& key) const {
        const uint32_t i = Find(key);
        if (i == slots_.size())
            throw std::out_of_range("FastMap::at");
        return slots_[i]->second;
    }
    V& operator[](const K& key) { return try_emplace(key).first->second; }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const K& key, Args&&... args) {
        bool inserted;
        const uint32_t i = Slot(key, inserted);
        if (inserted)
            slots_[i] = new value_type(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(std::forward<Args>(args)...));
        return {iterator(this, i, false), inserted};
    }
    template <typename M>
    std::pair<iterator, bool> insert_or_assign(const K& key, M&& value) {
        auto result = try_emplace(key, std::forward<M>(value));
        if (!result.second)
            result.first->second = std::forward<M>(value);
        return result;
    }
    template <typename M>
    std::pair<iterator, bool> emplace(const K& key, M&& value) {
        return try_emplace(key, std::forward<M>(value));
    }
    std::pair<iterator, bool> insert(const value_type& item) { return try_emplace(item.first, item.second); }

    size_t erase(const K& key) {
        const uint32_t i = Find(key);
        if (i == slots_.size())
            return 0;
        Remove(i);
        return 1;
    }
    iterator erase(const_iterator position) {
        const uint32_t i = position.i_;
        Remove(i);
        return iterator(this, i + 1, true);
    }
    iterator erase(iterator position) { return erase(const_iterator(position)); }

    // Erases every entry `pred(entry)` holds for; returns how many.
    template <typename Pred>
    size_t erase_if(Pred pred) {
        const uint32_t before = size_;
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (Live(slots_[i]) && pred(*slots_[i]))
                Remove(i);
        return before - size_;
    }

  private:
    std::vector<value_type*> slots_;
    uint32_t size_ = 0, used_ = 0;  // live entries; live entries plus tombstones
    uint32_t shift_ = 32;           // 32 - log2(capacity)

    static uint32_t Hash(const K& key) {
        const uint64_t x = uint64_t(H{}(key));
        // Fibonacci hashing: the top bits of the product are the bucket.
        return (uint32_t(x) ^ uint32_t(x >> 32) * 0x85EBCA6Bu) * 0x9E3779B1u;
    }
    uint32_t Mask() const { return uint32_t(slots_.size()) - 1; }
    uint32_t Home(const K& key) const { return shift_ >= 32 ? 0 : Hash(key) >> shift_; }

    // The key's slot, or slots_.size() when absent.
    uint32_t Find(const K& key) const {
        if (size_ == 0)
            return uint32_t(slots_.size());
        const uint32_t mask = Mask();
        for (uint32_t i = Home(key);; i = (i + 1) & mask) {
            const value_type* p = slots_[i];
            if (!p)
                return uint32_t(slots_.size());
            if (p != Tomb() && p->first == key)
                return i;
        }
    }

    // The key's slot; when absent, a free slot for it (`inserted`, still null).
    uint32_t Slot(const K& key, bool& inserted) {
        if ((used_ + 1) * 2 > slots_.size())
            Rehash(size_ * 4 + 2 > slots_.size() ? (slots_.empty() ? 8 : slots_.size() * 2) : slots_.size());
        const uint32_t mask = Mask();
        uint32_t tomb = UINT32_MAX;
        for (uint32_t i = Home(key);; i = (i + 1) & mask) {
            value_type* p = slots_[i];
            if (!p) {
                inserted = true;
                ++size_;
                if (tomb != UINT32_MAX)
                    return tomb;
                ++used_;
                return i;
            }
            if (p == Tomb()) {
                if (tomb == UINT32_MAX)
                    tomb = i;
            } else if (p->first == key) {
                inserted = false;
                return i;
            }
        }
    }

    void Remove(uint32_t i) {
        delete slots_[i];
        slots_[i] = Tomb();
        --size_;
    }

    void Rehash(size_t capacity) {
        std::vector<value_type*> old;
        old.swap(slots_);
        slots_.assign(capacity, nullptr);
        uint32_t bits = 0;
        while ((size_t(1) << bits) < capacity)
            ++bits;
        shift_ = 32 - bits;
        used_ = size_;
        const uint32_t mask = Mask();
        for (value_type* p : old) {
            if (!Live(p))
                continue;
            uint32_t i = Home(p->first);
            while (slots_[i])
                i = (i + 1) & mask;
            slots_[i] = p;
        }
    }

    void Swap(FastMap& other) noexcept {
        slots_.swap(other.slots_);
        std::swap(size_, other.size_);
        std::swap(used_, other.used_);
        std::swap(shift_, other.shift_);
    }
};

// The set form of FastMap, with its hashing, iteration and erase rules.
template <typename K, typename H = FastMapHash<K>>
class FastSet {
    struct Empty {};
    using Map = FastMap<K, Empty, H>;

  public:
    using key_type = K;
    using value_type = K;
    using size_type = size_t;

    class const_iterator {
        typename Map::const_iterator it_;
        friend class FastSet;

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = K;
        using difference_type = ptrdiff_t;
        using pointer = const K*;
        using reference = const K&;
        const_iterator() = default;
        explicit const_iterator(typename Map::const_iterator it) : it_(it) {}
        reference operator*() const { return it_->first; }
        pointer operator->() const { return &it_->first; }
        const_iterator& operator++() {
            ++it_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator old = *this;
            ++it_;
            return old;
        }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.it_ == b.it_; }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) { return a.it_ != b.it_; }
    };
    using iterator = const_iterator;

    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    const_iterator begin() const { return const_iterator(map_.begin()); }
    const_iterator end() const { return const_iterator(map_.end()); }
    void clear() { map_.clear(); }
    void reserve(size_t n) { map_.reserve(n); }

    const_iterator find(const K& key) const { return const_iterator(map_.find(key)); }
    bool contains(const K& key) const { return map_.contains(key); }
    size_t count(const K& key) const { return map_.count(key); }

    std::pair<const_iterator, bool> insert(const K& key) {
        const auto [it, inserted] = map_.try_emplace(key);
        return {const_iterator(it), inserted};
    }
    size_t erase(const K& key) { return map_.erase(key); }
    const_iterator erase(const_iterator position) { return const_iterator(map_.erase(position.it_)); }
    template <typename Pred>
    size_t erase_if(Pred pred) {
        return map_.erase_if([&](const auto& entry) { return pred(entry.first); });
    }

  private:
    Map map_;
};
