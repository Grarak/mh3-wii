// The Wii's network IOS devices for online play (ios_net.cpp): /dev/net/ip/top
// (the SO socket library), /dev/net/ssl, /dev/net/ncd/manage and
// /dev/net/kd/request (once online play started), after Dolphin's
// Core/IOS/Network. ios.cpp routes their requests here.
//
// A request that would block (a blocking socket's connect, receive or poll,
// a TLS handshake, a name lookup) waits in the guest thread scheduler: other
// guest threads run until the game thread's device pump finds it done. An
// asynchronous request's reply comes from that pump.
#pragma once

#include <cstdint>
#include <string_view>

struct CpuContext;

namespace IosNet {
// Whether `path` is one of these devices.
bool IsDevice(std::string_view path);

// IOS_Open: 0 when the device can open, else an IOS error.
int32_t Open(std::string_view path);

void Close(std::string_view path);

int32_t Ioctl(CpuContext* cpu, std::string_view path, uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize);

// IOS_Ioctlv: `vectors` holds `inputs` then `outputs` {address, size} pairs.
int32_t Ioctlv(CpuContext* cpu, std::string_view path, uint32_t command, uint32_t inputs, uint32_t outputs, uint32_t vectors);

// The asynchronous forms: callback(result, argument) runs at a device pump.
void IoctlAsync(std::string_view path, uint32_t command, uint32_t in, uint32_t inSize, uint32_t out, uint32_t outSize, uint32_t callback, uint32_t argument);
void IoctlvAsync(std::string_view path, uint32_t command, uint32_t inputs, uint32_t outputs, uint32_t vectors, uint32_t callback, uint32_t argument);

// Device pump (game thread): replies to the asynchronous requests now done.
void Service();
} // namespace IosNet
