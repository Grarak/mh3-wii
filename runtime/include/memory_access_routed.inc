// The memory helpers of MemoryHw and MemoryAlias (memory_access.h), included
// inside each after its Load and Store: every access goes through those, by
// its guest address. No include guard.
MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t a) {
    return Load<uint8_t>(a);
}

MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t a) {
    return Load<uint16_t>(a);
}

MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t a) {
    return Load<uint32_t>(a);
}

MKW_MEMORY_FORCE_INLINE uint64_t FlatRead64(uint32_t a) {
    return Load<uint64_t>(a);
}

MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t a) {
    return PpcSingleBitsToFloat(Load<uint32_t>(a));
}

MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t a) {
    const uint64_t bits = Load<uint64_t>(a);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t a, uint8_t v) {
    Store<uint8_t>(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t a, uint16_t v) {
    Store<uint16_t>(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t a, uint32_t v) {
    Store<uint32_t>(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite64(uint32_t a, uint64_t v) {
    Store<uint64_t>(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t a, double v) {
    Store<uint32_t>(a, ConvertPpcDoubleToSingleBits(v));
}

MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t a, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    Store<uint64_t>(a, bits);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t a, uint8_t v) {
    FlatWrite8(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t a, uint16_t v) {
    FlatWrite16(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t a, uint32_t v) {
    FlatWrite32(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam64(uint32_t a, uint64_t v) {
    FlatWrite64(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t a, double v) {
    FlatWriteFloat32(a, v);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t a, double v) {
    FlatWriteFloat64(a, v);
}

inline uint8_t ReadStack8(uint32_t a) {
    return FlatRead8(a);
}

inline uint16_t ReadStack16(uint32_t a) {
    return FlatRead16(a);
}

inline uint32_t ReadStack32(uint32_t a) {
    return FlatRead32(a);
}

inline uint64_t ReadStack64(uint32_t a) {
    return FlatRead64(a);
}

inline void WriteStack8(uint32_t a, uint8_t v) {
    FlatWrite8(a, v);
}

inline void WriteStack16(uint32_t a, uint16_t v) {
    FlatWrite16(a, v);
}

inline void WriteStack32(uint32_t a, uint32_t v) {
    FlatWrite32(a, v);
}

inline void WriteStack64(uint32_t a, uint64_t v) {
    FlatWrite64(a, v);
}

inline void WriteStackFloat32(uint32_t a, double v) {
    FlatWriteFloat32(a, v);
}

inline void WriteStackFloat64(uint32_t a, double v) {
    FlatWriteFloat64(a, v);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE T PsqLoad(uint32_t address) {
    return Load<T>(address);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void PsqStore(uint32_t address, T value) {
    Store<T>(address, value);
}

// The host address of a paired-single access (ppc_isa_quantized_access.inc).
MKW_MEMORY_FORCE_INLINE uint8_t* PsqHost(uint32_t address) {
    return GuestFlat::Host(address);
}
