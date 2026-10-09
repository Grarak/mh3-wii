#pragma once

#include <cstdint>
#include <string>

// Host implementation of Dolphin's /dev/dolphin Discord contract. The guest
// sends only strings and big-endian integer fields; the IPC wire protocol is
// owned here so translated game code never needs host SDK headers.
namespace DiscordPresence {

struct Activity {
    std::string details;
    std::string state;
    std::string largeImageKey;
    std::string largeImageText;
    std::string smallImageKey;
    std::string smallImageText;
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    uint32_t partySize = 0;
    uint32_t partyMax = 0;
};

void Initialize(const std::string& basicClientId, const std::string& basicTitle);
void SetClient(const std::string& clientId);
void SetActivity(Activity activity);
void Reset();
void Shutdown();

} // namespace DiscordPresence
