// Paired-single loads and stores, included by isa/ppc_isa_quantized.h twice:
// once as namespace PsqFlat with MKW_PSQ_MEM = MemoryInline (plain flat guest
// memory, used by all translated code), once inside namespace MemoryHw with
// MKW_PSQ_MEM = MemoryHw (hardware addresses reach the device models) for the
// functions in runtime/src/hardware_access.inc. No include guard.

template <uint32_t W, uint32_t I, bool Stack>
inline double PPC_PsqLStateFallback(uint32_t gqr, uint32_t addr);
template <uint32_t W, uint32_t I, bool Stack>
inline void PPC_PsqStStateFallback(uint32_t gqr, uint32_t addr, double value);

// The generic psq_l/psq_st of this namespace, with the thread's GQR I.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PsqLCurrentGqr(uint32_t addr)
{
    CpuContext* cpu = TryGetCpuContext();

    if (!cpu) [[unlikely]] std::abort();
    return PPC_PsqLStateFallback<W, I, false>(cpu->gqr[I], addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PsqStCurrentGqr(uint32_t addr, double value)
{
    CpuContext* cpu = TryGetCpuContext();

    if (!cpu) [[unlikely]] std::abort();
    PPC_PsqStStateFallback<W, I, false>(cpu->gqr[I], addr, value);
}

template <typename T>
inline T PpcReadUnpairedPsqInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        return static_cast<T>(MKW_PSQ_MEM::PsqLoad<uint8_t>(addr));
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(MKW_PSQ_MEM::PsqLoad<uint16_t>(addr));
    }
    else
    {
        return static_cast<T>(MKW_PSQ_MEM::PsqLoad<uint32_t>(addr));
    }
}

template <typename T>
inline std::pair<T, T> PpcReadPairPsqInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = MKW_PSQ_MEM::PsqLoad<uint16_t>(addr);
        return { static_cast<T>(packed >> 8), static_cast<T>(packed) };
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = MKW_PSQ_MEM::PsqLoad<uint32_t>(addr);
        return { static_cast<T>(packed >> 16), static_cast<T>(packed) };
    }
    else
    {
        const uint64_t packed = MKW_PSQ_MEM::PsqLoad<uint64_t>(addr);
        return { static_cast<T>(packed >> 32), static_cast<T>(packed) };
    }
}

template <typename T>
inline void PpcWriteUnpairedPsqInline(uint32_t addr, T value)
{
    if constexpr (sizeof(T) == 1)
    {
        MKW_PSQ_MEM::PsqStore<uint8_t>(addr, static_cast<uint8_t>(value));
    }
    else if constexpr (sizeof(T) == 2)
    {
        MKW_PSQ_MEM::PsqStore<uint16_t>(addr, static_cast<uint16_t>(value));
    }
    else
    {
        MKW_PSQ_MEM::PsqStore<uint32_t>(addr, static_cast<uint32_t>(value));
    }
}

template <typename T>
inline void PpcWritePairPsqInline(uint32_t addr, T first, T second)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = (static_cast<uint16_t>(first) << 8) | static_cast<uint16_t>(second);
        MKW_PSQ_MEM::PsqStore<uint16_t>(addr, packed);
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
        MKW_PSQ_MEM::PsqStore<uint32_t>(addr, packed);
    }
    else
    {
        const uint64_t packed = (static_cast<uint64_t>(first) << 32) | static_cast<uint64_t>(second);
        MKW_PSQ_MEM::PsqStore<uint64_t>(addr, packed);
    }
}

inline double PpcLoadPairPsqFloatFastInline(uint32_t addr)
{
    if (!MKW_PSQ_MEM::PsqHardware(addr)) [[likely]]
        return PpcLoadPairPsqFloatFromHostInline(GuestFlat::Host(addr));
    return PpcBitCastToDoubleInline(
        PpcLoadPairPsqFloatBitsPackedInline(MKW_PSQ_MEM::PsqLoad<uint64_t>(addr)));
}

inline double PpcLoadSinglePsqFloatFastInline(uint32_t addr)
{
    const uint32_t raw = PpcReadUnpairedPsqInline<uint32_t>(addr);
    return PpcMakePairedResultInline(
        PpcBitCastToFloatInline(raw),
        1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerFastInline(uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1)
    {
        const uint16_t packed = MKW_PSQ_MEM::PsqLoad<uint16_t>(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else if constexpr (sizeof(ValueType) == 2)
    {
        const uint32_t packed = MKW_PSQ_MEM::PsqLoad<uint32_t>(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else
    {
        const uint64_t packed = MKW_PSQ_MEM::PsqLoad<uint64_t>(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedFastInline(uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    const UnsignedType value = PpcReadUnpairedPsqInline<UnsignedType>(addr);
    return PpcMakePairedResultInline(
        PpcDequantizePsqInline(static_cast<SignedType>(value), scale),
        1.0f).d;
}

inline void PpcStorePairPsqFloatFastInline(uint32_t addr, double value)
{
    if (!MKW_PSQ_MEM::PsqHardware(addr)) [[likely]]
    {
        PpcStorePairPsqFloatToHostInline(GuestFlat::Host(addr), value);
        return;
    }

    MKW_PSQ_MEM::PsqStore<uint64_t>(
        addr, PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value)));
}

inline uint8_t PpcQuantizePsqU8Scale61Inline(float value)
{
    const float scaled = value * 0.125f;

    if (!(scaled > 0.0f))
    {
        return 0;
    }

    if (scaled >= 255.0f)
    {
        return 255;
    }

    return static_cast<uint8_t>(scaled);
}

// GQR U8 scale 61 is common in paired stores. Quantize both payload lanes as
// one native vector so the normal finite path does not branch once per lane.
// MAXPS with zero as the second operand also maps either NaN lane to zero,
// matching PpcQuantizePsqU8Scale61Inline's !(scaled > 0) rule.
inline uint16_t PpcQuantizePairPsqU8Scale61PackedInline(double value)
{
#if defined(__x86_64__)
    const __m128 scaled = _mm_mul_ps(PpcPsToM128Inline(value), _mm_set1_ps(0.125f));
    const __m128 nonNegative = _mm_max_ps(scaled, _mm_setzero_ps());
    const __m128 clamped = _mm_min_ps(nonNegative, _mm_set1_ps(255.0f));
    const __m128i lanes32 = _mm_cvttps_epi32(clamped);
    const __m128i lanes16 = _mm_packs_epi32(lanes32, lanes32);
    const __m128i lanes8 = _mm_packus_epi16(lanes16, lanes16);

    // Native lane 0 is ps1 and lane 1 is ps0. Packing to the low uint16_t
    // therefore produces the guest-order numeric value (ps0 << 8) | ps1.
    return static_cast<uint16_t>(_mm_cvtsi128_si32(lanes8));
#elif defined(__aarch64__) || defined(__arm__)
    // Equivalent to two calls of the already-portable scalar quantizer above
    // (its own !(scaled > 0) rule maps NaN to 0, matching what MAXPS-with-zero
    // does on the x86 path per the comment there), packed the same way the
    // fallback path just below already does for the non-SIMD case.
    PPC_FPR fpr{};
    fpr.d = value;
    const uint8_t q0 = PpcQuantizePsqU8Scale61Inline(fpr.paired.ps0);
    const uint8_t q1 = PpcQuantizePsqU8Scale61Inline(fpr.paired.ps1);
    return static_cast<uint16_t>((static_cast<uint16_t>(q0) << 8) | static_cast<uint16_t>(q1));
#endif
}

// The hardware store, off the leaf path, so the conversion is not replicated
// at every compile-time-known PSQ store.
MKW_PPC_NO_INLINE inline void PpcStorePairPsqU8Scale61Fallback(
    uint32_t addr, double value)
{
    PPC_FPR fpr{};
    fpr.d = value;
    const uint16_t packed = static_cast<uint16_t>(
        (static_cast<uint16_t>(PpcQuantizePsqU8Scale61Inline(fpr.paired.ps0)) << 8) |
        static_cast<uint16_t>(PpcQuantizePsqU8Scale61Inline(fpr.paired.ps1)));
    MKW_PSQ_MEM::PsqStore<uint16_t>(addr, packed);
}

// Quantize both lanes as one vector and store them directly; a hardware
// address (MemoryHw only) takes the cold path.
MKW_PPC_FORCE_INLINE void PpcStorePairPsqU8Scale61FastInline(
    uint32_t addr, double value)
{
    if (MKW_PSQ_MEM::PsqHardware(addr)) [[unlikely]]
    {
        PpcStorePairPsqU8Scale61Fallback(addr, value);
        return;
    }

    const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
    BigEndian::Write16(GuestFlat::Host(addr), packed);
}

MKW_PPC_FORCE_INLINE void PpcStorePairPsqU8Scale61ResolvedInline(
    uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost)
    {
        PpcStorePairPsqU8Scale61Fallback(addr, value);
        return;
    }

    const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
    BigEndian::Write16(resolvedHost + offset, packed);
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedFastInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;

    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            PpcStorePairPsqU8Scale61FastInline(addr, value);
            return;
        }
    }

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        const auto second = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale)));
        PpcWritePairPsqInline(addr, first, second);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        const auto second = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale));
        PpcWritePairPsqInline(addr, first, second);
    }
}

inline void PpcStoreSinglePsqFloatFastInline(uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(
        PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    PpcWriteUnpairedPsqInline(addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedFastInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        PpcWriteUnpairedPsqInline(addr, first);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        PpcWriteUnpairedPsqInline(addr, first);
    }
}

inline double PpcLoadPairPsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    // A resolved range is already the proof; ReadResolved64 only
    // falls back when the host pointer is null.
    if (resolvedHost != nullptr) [[likely]]
        return PpcLoadPairPsqFloatFromHostInline(resolvedHost + offset);
    return PpcBitCastToDoubleInline(PpcLoadPairPsqFloatBitsPackedInline(
        MKW_PSQ_MEM::PsqLoad<uint64_t>(addr)));
}

inline double PpcLoadSinglePsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    const uint32_t raw = MKW_PSQ_MEM::ReadResolved32(resolvedHost, offset, addr);
    return PpcMakePairedResultInline(PpcBitCastToFloatInline(raw), 1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerResolvedInline(uint8_t* resolvedHost, uint32_t offset,
                                                   uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1) {
        const uint16_t packed = MKW_PSQ_MEM::ReadResolved16(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    } else if constexpr (sizeof(ValueType) == 2) {
        const uint32_t packed = MKW_PSQ_MEM::ReadResolved32(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    } else {
        const uint64_t packed = MKW_PSQ_MEM::ReadResolved64(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    UnsignedType value;

    if constexpr (sizeof(UnsignedType) == 1) value = MKW_PSQ_MEM::ReadResolved8(resolvedHost, offset, addr);
    else if constexpr (sizeof(UnsignedType) == 2) value = MKW_PSQ_MEM::ReadResolved16(resolvedHost, offset, addr);
    else value = MKW_PSQ_MEM::ReadResolved32(resolvedHost, offset, addr);
    return PpcMakePairedResultInline(PpcDequantizePsqInline(static_cast<SignedType>(value), scale), 1.0f).d;
}

inline void PpcStorePairPsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    // Mirrors WriteResolved64: the resolved range is the proof,
    // and only a null host takes the original-address fallback.
    if (resolvedHost != nullptr) [[likely]]
    {
        PpcStorePairPsqFloatToHostInline(resolvedHost + offset, value);
        return;
    }

    MKW_PSQ_MEM::PsqStore<uint64_t>(
        addr, PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value)));
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{}; fpr.d = value;

    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            PpcStorePairPsqU8Scale61ResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    }

    using Unsigned = std::make_unsigned_t<QuantizedType>;
    const auto quantize = [scale](float lane) -> Unsigned {
        return static_cast<Unsigned>(static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(lane, scale)));
    };

    const Unsigned first = quantize(fpr.paired.ps0);
    const Unsigned second = quantize(fpr.paired.ps1);

    if constexpr (sizeof(QuantizedType) == 1)
        MKW_PSQ_MEM::WriteResolved16(resolvedHost, offset, addr, (static_cast<uint16_t>(first) << 8) | second);
    else if constexpr (sizeof(QuantizedType) == 2)
        MKW_PSQ_MEM::WriteResolved32(resolvedHost, offset, addr, (static_cast<uint32_t>(first) << 16) | second);
    else
        MKW_PSQ_MEM::WriteResolved64(resolvedHost, offset, addr, (static_cast<uint64_t>(first) << 32) | second);
}

inline void PpcStoreSinglePsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    MKW_PSQ_MEM::WriteResolved32(resolvedHost, offset, addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{}; fpr.d = value;
    using Unsigned = std::make_unsigned_t<QuantizedType>;
    const Unsigned first = static_cast<Unsigned>(static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));

    if constexpr (sizeof(QuantizedType) == 1) MKW_PSQ_MEM::WriteResolved8(resolvedHost, offset, addr, first);
    else if constexpr (sizeof(QuantizedType) == 2) MKW_PSQ_MEM::WriteResolved16(resolvedHost, offset, addr, first);
    else MKW_PSQ_MEM::WriteResolved32(resolvedHost, offset, addr, first);
}

template <typename T>
inline T PpcReadUnpairedPsqStackInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        return static_cast<T>(MKW_PSQ_MEM::ReadStack8(addr));
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(MKW_PSQ_MEM::ReadStack16(addr));
    }
    else
    {
        return static_cast<T>(MKW_PSQ_MEM::ReadStack32(addr));
    }
}

template <typename T>
inline void PpcWriteUnpairedPsqStackInline(uint32_t addr, T value)
{
    if constexpr (sizeof(T) == 1)
    {
        MKW_PSQ_MEM::WriteStack8(addr, static_cast<uint8_t>(value));
    }
    else if constexpr (sizeof(T) == 2)
    {
        MKW_PSQ_MEM::WriteStack16(addr, static_cast<uint16_t>(value));
    }
    else
    {
        MKW_PSQ_MEM::WriteStack32(addr, static_cast<uint32_t>(value));
    }
}

template <typename T>
inline void PpcWritePairPsqStackInline(uint32_t addr, T first, T second)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = (static_cast<uint16_t>(first) << 8) | static_cast<uint16_t>(second);
        MKW_PSQ_MEM::WriteStack16(addr, packed);
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
        MKW_PSQ_MEM::WriteStack32(addr, packed);
    }
    else
    {
        const uint64_t packed = (static_cast<uint64_t>(first) << 32) | static_cast<uint64_t>(second);
        MKW_PSQ_MEM::WriteStack64(addr, packed);
    }
}

inline double PpcLoadPairPsqFloatStackInline(uint32_t addr)
{
    return PpcLoadPairPsqFloatFastInline(addr);
}

inline double PpcLoadSinglePsqFloatStackInline(uint32_t addr)
{
    const uint32_t raw = PpcReadUnpairedPsqStackInline<uint32_t>(addr);
    return PpcMakePairedResultInline(PpcBitCastToFloatInline(raw), 1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerStackInline(uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1)
    {
        const uint16_t packed = MKW_PSQ_MEM::ReadStack16(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else if constexpr (sizeof(ValueType) == 2)
    {
        const uint32_t packed = MKW_PSQ_MEM::ReadStack32(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else
    {
        const uint64_t packed = MKW_PSQ_MEM::ReadStack64(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedStackInline(uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    const UnsignedType value = PpcReadUnpairedPsqStackInline<UnsignedType>(addr);
    return PpcMakePairedResultInline(
        PpcDequantizePsqInline(static_cast<SignedType>(value), scale),
        1.0f).d;
}

inline void PpcStorePairPsqFloatStackInline(uint32_t addr, double value)
{
    PpcStorePairPsqFloatFastInline(addr, value);
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedStackInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;

    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
            MKW_PSQ_MEM::WriteStack16(addr, packed);
            return;
        }
    }

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        const auto second = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale)));
        PpcWritePairPsqStackInline(addr, first, second);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        const auto second = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale));
        PpcWritePairPsqStackInline(addr, first, second);
    }
}

inline void PpcStoreSinglePsqFloatStackInline(uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(
        PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    PpcWriteUnpairedPsqStackInline(addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedStackInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        PpcWriteUnpairedPsqStackInline(addr, first);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        PpcWriteUnpairedPsqStackInline(addr, first);
    }
}


// Cold generic body for psq_l. The complete quantization switch lives in one
// out-of-line copy so PPC_PsqLInline's float fast path can be force-inlined at
// guest call sites without spilling live registers around a call.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLGeneric(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        switch (gqr & 0xFFFF0000u)
        {
        case 0x00000000u:
return PpcLoadPairPsqFloatFastInline(addr);
        case 0x00040000u:
return PpcLoadPairPsqIntegerFastInline<uint8_t>(addr);
        case 0x00050000u:
return PpcLoadPairPsqIntegerFastInline<uint16_t>(addr);
        case 0x00060000u:
return PpcLoadPairPsqIntegerFastInline<int8_t>(addr);
        case 0x00070000u:
return PpcLoadPairPsqIntegerFastInline<int16_t>(addr);
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = (gqr >> 16) & 0x7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;

        switch (type)
        {
        case 0u:
return PpcLoadSinglePsqFloatFastInline(addr);
        case 4u:
return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
        case 5u:
return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
        case 6u:
return PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
        case 7u:
return PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
        default:
            break;
        }
    }

    return PsqLCurrentGqr<W, I>(addr);
}

// Hot wrapper: the float encoding (quantization type 0) is what the SDK leaves
// in the GQRs for the overwhelming majority of psq_l sites, so it is tested with
// one compare and handled inline. Every other encoding, including the reserved
// ones that must reach PPC_PsqL, tail-calls the cold generic body above.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLInline(CpuContext* cpu, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");

    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatFastInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqr >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatFastInline(addr);
        }
    }

    return PPC_PsqLGeneric<W, I>(gqr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLInline(uint32_t addr)
{
    return PPC_PsqLInline<W, I>(TryGetCpuContext(), addr);
}

// GQR-hoisting entry points (audit T-GQR). Identical bodies to PPC_PsqLInline/PPC_PsqStInline
// above, but the GQR value arrives as an argument instead of being reloaded from ctx->gqr[I]
// at every access, since pointer indirection otherwise forces a reload after every intervening
// store. I is only consulted for reserved encodings that tail-call PPC_PsqL/PPC_PsqSt.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE double PPC_PsqLGqrInline(uint32_t gqrValue, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqrValue & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatFastInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqrValue >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatFastInline(addr);
        }
    }

    return PPC_PsqLGeneric<W, I>(gqrValue, addr);
}

// Context-carrying form, for emission sites that still hand the CpuContext to
// every helper. The context is not read; the null check is retained so a broken
// call site fails the same way the ctx->gqr[I] form does.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE double PPC_PsqLGqrInline(CpuContext* cpu, uint32_t gqrValue, uint32_t addr)
{
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    return PPC_PsqLGqrInline<W, I>(gqrValue, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownInline(CpuContext*, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatFastInline(addr);
        else return PpcLoadSinglePsqFloatFastInline(addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<int8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<int16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
    }
    else
    {
        return PsqLCurrentGqr<W, I>(addr);
    }
}

// Cold generic body for the stack-form psq_l, mirroring PPC_PsqLGeneric: the
// complete quantization switch lives in one out-of-line copy so the hot
// wrapper's float path can be force-inlined at guest call sites without
// spilling live registers around a call.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLStackGeneric(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        switch (gqr & 0xFFFF0000u)
        {
        case 0x00000000u:
return PpcLoadPairPsqFloatStackInline(addr);
        case 0x00040000u:
return PpcLoadPairPsqIntegerStackInline<uint8_t>(addr);
        case 0x00050000u:
return PpcLoadPairPsqIntegerStackInline<uint16_t>(addr);
        case 0x00060000u:
return PpcLoadPairPsqIntegerStackInline<int8_t>(addr);
        case 0x00070000u:
return PpcLoadPairPsqIntegerStackInline<int16_t>(addr);
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = (gqr >> 16) & 0x7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;

        switch (type)
        {
        case 0u:
return PpcLoadSinglePsqFloatStackInline(addr);
        case 4u:
return PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale);
        case 5u:
return PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale);
        case 6u:
return PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale);
        case 7u:
return PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale);
        default:
            break;
        }
    }

    return PsqLCurrentGqr<W, I>(addr);
}

// Hot wrapper: the float encoding (quantization type 0) is what the SDK leaves
// in the GQRs for the overwhelming majority of psq_l sites, so it is tested with
// one compare and handled inline. Every other encoding, including the reserved
// ones that must reach PPC_PsqL, tail-calls the cold generic body above.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLStackInline(CpuContext* cpu, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");

    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatStackInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqr >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatStackInline(addr);
        }
    }

    return PPC_PsqLStackGeneric<W, I>(gqr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLStackInline(uint32_t addr)
{
    return PPC_PsqLStackInline<W, I>(TryGetCpuContext(), addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownStackInline(CpuContext*, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatStackInline(addr);
        else return PpcLoadSinglePsqFloatStackInline(addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<int8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<int16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale);
    }
    else
    {
        return PsqLCurrentGqr<W, I>(addr);
    }
}

// Cold generic body for psq_st. See PPC_PsqLGeneric: the full switch stays out
// of line so the float fast path in PPC_PsqStInline can be force-inlined.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStGeneric(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        switch (gqr & 0x0000FFFFu)
        {
        case 0x0000u:
PpcStorePairPsqFloatFastInline(addr, value);
            return;
        case 0x0004u:
PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 0u);
            return;
        case 0x0005u:
PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, 0u);
            return;
        case 0x0006u:
PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, 0u);
            return;
        case 0x0007u:
PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, 0u);
            return;
        case 0x3D04u:
PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 61u);
            return;
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = gqr & 0x7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;

        switch (type)
        {
        case 0u:
PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        case 4u:
PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
            return;
        case 5u:
PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale);
            return;
        case 6u:
PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale);
            return;
        case 7u:
PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale);
            return;
        default:
            break;
        }
    }

    PsqStCurrentGqr<W, I>(addr, value);
}

// Hot wrapper: store type 0 (float) is the common GQR setup, tested with a
// single compare and handled inline. Quantized encodings (including the U8
// scale-61 pair case) and reserved encodings go to the cold generic body.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(CpuContext* cpu, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");

    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqr & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatFastInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqr & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        }
    }

    PPC_PsqStGeneric<W, I>(gqr, addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(uint32_t addr, double value)
{
    PPC_PsqStInline<W, I>(TryGetCpuContext(), addr, value);
}

// GQR-hoisting store entry points. See PPC_PsqLGqrInline above for why the GQR
// value is passed in and what the template parameters still mean.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(uint32_t gqrValue, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqrValue & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatFastInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqrValue & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        }
    }

    PPC_PsqStGeneric<W, I>(gqrValue, addr, value);
}

template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(CpuContext* cpu, uint32_t gqrValue, uint32_t addr, double value)
{
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    PPC_PsqStGqrInline<W, I>(gqrValue, addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownInline(CpuContext*, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatFastInline(addr, value);
        else PpcStoreSinglePsqFloatFastInline(addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale);
    }
    else
    {
        PsqStCurrentGqr<W, I>(addr, value);
    }
}

// Cold generic body for the stack-form psq_st. See PPC_PsqStGeneric: the full
// switch stays out of line so the float fast path in the hot wrapper below can
// be force-inlined.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStStackGeneric(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");

    if constexpr (W == 0u)
    {
        switch (gqr & 0x0000FFFFu)
        {
        case 0x0000u:
PpcStorePairPsqFloatStackInline(addr, value);
            return;
        case 0x0004u:
PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 0u);
            return;
        case 0x0005u:
PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, 0u);
            return;
        case 0x0006u:
PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, 0u);
            return;
        case 0x0007u:
PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, 0u);
            return;
        case 0x3D04u:
PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 61u);
            return;
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = gqr & 0x7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;

        switch (type)
        {
        case 0u:
PpcStoreSinglePsqFloatStackInline(addr, value);
            return;
        case 4u:
PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
            return;
        case 5u:
PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale);
            return;
        case 6u:
PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale);
            return;
        case 7u:
PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale);
            return;
        default:
            break;
        }
    }

    PsqStCurrentGqr<W, I>(addr, value);
}

// Hot wrapper: store type 0 (float) is the common GQR setup, tested with a
// single compare and handled inline. Quantized encodings (including the U8
// scale-61 pair case) and reserved encodings go to the cold generic body.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(CpuContext* cpu, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");

    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqr & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatStackInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqr & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatStackInline(addr, value);
            return;
        }
    }

    PPC_PsqStStackGeneric<W, I>(gqr, addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(uint32_t addr, double value)
{
    PPC_PsqStStackInline<W, I>(TryGetCpuContext(), addr, value);
}

// Context-free PSQ entries for translated regions which own GQR state as an
// ordinary native value. All architecturally valid quantization encodings are
// handled directly; reserved encodings retain the generic helper's abort.
template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLStateFallback(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);
    const uint32_t type = (gqr >> 16) & 0x7u;
    const uint32_t scale = (gqr >> 24) & 0x3Fu;

    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: return Stack ? PpcLoadPairPsqFloatStackInline(addr) : PpcLoadPairPsqFloatFastInline(addr);
        case 4u: return Stack ? PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, scale);
        case 5u: return Stack ? PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, scale);
        case 6u: return Stack ? PpcLoadPairPsqIntegerStackInline<int8_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<int8_t>(addr, scale);
        case 7u: return Stack ? PpcLoadPairPsqIntegerStackInline<int16_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<int16_t>(addr, scale);
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: return Stack ? PpcLoadSinglePsqFloatStackInline(addr) : PpcLoadSinglePsqFloatFastInline(addr);
        case 4u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
        case 5u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
        case 6u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
        case 7u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
        default: std::abort();
        }
    }
}

// Keep the normal explicit-state path small and directly optimizable. Exact
// unscaled encodings cover the SDK's common GQR setup; scaled and reserved
// encodings retain the complete implementation in one cold outlined body.
template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE double PPC_PsqLStateInline(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);

    switch (gqr & 0xFFFF0000u)
    {
    case 0x00000000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqFloatStackInline(addr) : PpcLoadPairPsqFloatFastInline(addr);
        else return Stack ? PpcLoadSinglePsqFloatStackInline(addr) : PpcLoadSinglePsqFloatFastInline(addr);
    case 0x00040000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, 0u);
    case 0x00050000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, 0u);
    case 0x00060000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<int8_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<int8_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, 0u);
    case 0x00070000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<int16_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<int16_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, 0u);
    default:
        return PPC_PsqLStateFallback<W, I, Stack>(gqr, addr);
    }
}

template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStStateFallback(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);
    const uint32_t type = gqr & 0x7u;
    const uint32_t scale = (gqr >> 8) & 0x3Fu;

    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: Stack ? PpcStorePairPsqFloatStackInline(addr, value) : PpcStorePairPsqFloatFastInline(addr, value); return;
        case 4u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: if constexpr (Stack) PpcStoreSinglePsqFloatStackInline(addr, value); else PpcStoreSinglePsqFloatFastInline(addr, value); return;
        case 4u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE void PPC_PsqStStateInline(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);

    switch (gqr & 0xFFFFu)
    {
    case 0x0000u:
        if constexpr (W == 0u) Stack ? PpcStorePairPsqFloatStackInline(addr, value) : PpcStorePairPsqFloatFastInline(addr, value);
        else Stack ? PpcStoreSinglePsqFloatStackInline(addr, value) : PpcStoreSinglePsqFloatFastInline(addr, value);
        return;
    case 0x0004u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, 0u); }
        return;
    case 0x0005u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, 0u); }
        return;
    case 0x0006u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, 0u); }
        return;
    case 0x0007u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, 0u); }
        return;
    default:
        PPC_PsqStStateFallback<W, I, Stack>(gqr, addr, value);
        return;
    }
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLResolvedStateInline(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);

    if (!resolvedHost) [[unlikely]] return PPC_PsqLStateInline<W, I, false>(gqr, addr);
    const uint32_t type = (gqr >> 16) & 0x7u;
    const uint32_t scale = (gqr >> 24) & 0x3Fu;

    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedStateInline(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);

    if (!resolvedHost) [[unlikely]]
    {
        PPC_PsqStStateInline<W, I, false>(gqr, addr, value);
        return;
    }

    const uint32_t type = gqr & 0x7u;
    const uint32_t scale = (gqr >> 8) & 0x3Fu;

    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownStackInline(CpuContext*, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatStackInline(addr, value);
        else PpcStoreSinglePsqFloatStackInline(addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale);
    }
    else
    {
        PsqStCurrentGqr<W, I>(addr, value);
    }
}

// Cold generic body for the resolved-host psq_l. Only reached with a non-null
// resolvedHost; the null case keeps its original PPC_PsqL behaviour in the
// force-inlined wrapper below.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLResolvedGeneric(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    if constexpr (W == 0u) {
        switch (gqr & 0xFFFF0000u) {
        case 0x00000000u: return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        case 0x00040000u: return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr);
        case 0x00050000u: return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr);
        case 0x00060000u: return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr);
        case 0x00070000u: return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr);
        default: return PsqLCurrentGqr<W, I>(addr);
        }
    } else {
        const uint32_t type = (gqr >> 16) & 7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;

        switch (type) {
        case 0u: return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: return PsqLCurrentGqr<W, I>(addr);
        }
    }
}

// Hot wrapper: resolved host pointer plus the float GQR encoding is the case
// that dominates translated code, so it is a compare and a direct byteswapped
// access. Null host and every non-float encoding stay out of line.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                   uint32_t offset, uint32_t addr)
{
    if (!resolvedHost) [[unlikely]] return PsqLCurrentGqr<W, I>(addr);
    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u) {
        // Paired float: load type 0 with load scale 0.
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
            return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
    } else {
        // Scalar float ignores the load scale.
        if (((gqr >> 16) & 7u) == 0u) [[likely]]
            return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
    }

    return PPC_PsqLResolvedGeneric<W, I>(gqr, resolvedHost, offset, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLKnownResolvedFallback(
    uint32_t addr)
{
    return PPC_PsqLKnownInline<W, I, GQR>(nullptr, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                        uint32_t offset, uint32_t addr)
{
    if (!resolvedHost) [[unlikely]] return PPC_PsqLKnownResolvedFallback<W, I, GQR>(addr);
    static_assert(W <= 1u, "psq resolved load W must be 0 or 1");
    static_assert(I < 8u, "psq resolved load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        else return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
    }
    else
    {
        return PsqLCurrentGqr<W, I>(addr);
    }
}

// Cold generic body for the resolved-host psq_st. Only reached with a non-null
// resolvedHost; the null case keeps its original PPC_PsqSt behaviour in the
// force-inlined wrapper below.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStResolvedGeneric(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    if constexpr (W == 0u) {
        switch (gqr & 0xFFFFu) {
        case 0x0000u: PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 0x0004u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0005u: PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0006u: PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0007u: PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x3D04u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, 61u); return;
        default: PsqStCurrentGqr<W, I>(addr, value); return;
        }
    } else {
        const uint32_t type = gqr & 7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;

        switch (type) {
        case 0u: PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: PsqStCurrentGqr<W, I>(addr, value); return;
        }
    }
}

// Hot wrapper: resolved host pointer plus the float GQR encoding becomes a
// compare and a direct byteswapped store. Null host and every non-float
// encoding (including U8 scale 61) stay out of line.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                  uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost) [[unlikely]] { PsqStCurrentGqr<W, I>(addr, value); return; }
    const uint32_t gqr = cpu->gqr[I];

    if constexpr (W == 0u) {
        // Paired float: store type 0 with store scale 0.
        if ((gqr & 0xFFFFu) == 0x0000u) [[likely]] {
            PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    } else {
        // Scalar float ignores the store scale.
        if ((gqr & 7u) == 0u) [[likely]] {
            PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    }

    PPC_PsqStResolvedGeneric<W, I>(gqr, resolvedHost, offset, addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStKnownResolvedFallback(
    uint32_t addr, double value)
{
    PPC_PsqStKnownInline<W, I, GQR>(nullptr, addr, value);
}


template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                       uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost) [[unlikely]] { PPC_PsqStKnownResolvedFallback<W, I, GQR>(addr, value); return; }
    static_assert(W <= 1u, "psq resolved store W must be 0 or 1");
    static_assert(I < 8u, "psq resolved store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value);
        else PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
            else PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
            else PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale);
    }
    else
    {
        PsqStCurrentGqr<W, I>(addr, value);
    }
}
