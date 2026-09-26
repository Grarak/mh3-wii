#pragma once

#include "isa/big_endian.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace PadStatusContract {

inline constexpr std::size_t kGuestStatusSize = 0x0C;
using GuestStatus = std::array<uint8_t, kGuestStatusSize>;

struct Fields {
    uint16_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t substickX = 0;
    int8_t substickY = 0;
    uint8_t triggerLeft = 0;
    uint8_t triggerRight = 0;
    uint8_t analogA = 0;
    uint8_t analogB = 0;
    int8_t error = 0;
};

inline GuestStatus Encode(const Fields& fields)
{
    GuestStatus status{};
    BigEndian::Write16(status.data(), fields.buttons);
    status[0x02] = static_cast<uint8_t>(fields.stickX);
    status[0x03] = static_cast<uint8_t>(fields.stickY);
    status[0x04] = static_cast<uint8_t>(fields.substickX);
    status[0x05] = static_cast<uint8_t>(fields.substickY);
    status[0x06] = fields.triggerLeft;
    status[0x07] = fields.triggerRight;
    status[0x08] = fields.analogA;
    status[0x09] = fields.analogB;
    status[0x0A] = static_cast<uint8_t>(fields.error);
    return status;
}

} // namespace PadStatusContract

namespace WpadContract {

inline constexpr std::size_t kChannelCount = 4;
inline constexpr int32_t kStatusDisabled = 0;
inline constexpr int32_t kStatusReady = 3;
inline constexpr int32_t kErrorNoController = -1;
inline constexpr int32_t kErrorNotReady = -2;
inline constexpr int32_t kErrorBadChannel = -6;
inline constexpr int32_t kExtensionCore = 0;

class State {
public:
    void Initialize() { m_initialized = true; }
    bool IsInitialized() const { return m_initialized; }
    int32_t GetLibraryStatus() const { return m_initialized ? kStatusReady : kStatusDisabled; }

    int32_t GetDataFormat(uint32_t chan) const
    {
        if (chan >= kChannelCount) {
            return kErrorBadChannel;
        }
        return m_initialized ? 0 : kErrorNotReady;
    }

    int32_t SetDataFormat(uint32_t chan, int32_t format) const
    {
        (void)format;
        if (chan >= kChannelCount) {
            return kErrorBadChannel;
        }
        return m_initialized ? kErrorNoController : kErrorNotReady;
    }

private:
    bool m_initialized = false;
};

} // namespace WpadContract
