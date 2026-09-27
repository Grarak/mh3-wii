# IOS55 USB HID startup

MH3 opens `/dev/usb/hid` when entering its save-selection interface. The native
boundary reports HID v4 version `0x40001` and an empty Wii USB device list. Host
keyboard/gamepad input continues through the WPAD bridge; USB passthrough is not
implemented.

The first asynchronous device-change query writes the `0xFFFFFFFF` list terminator
and queues success. Subsequent queries remain pending. Shutdown or close releases
an outstanding request with cancellation; another enumeration after shutdown can
complete. Duplicate outstanding subscriptions return busy. Transfers to absent
devices return not-found, and malformed request buffers return invalid-argument.
Unknown commands still fail explicitly.

Protocol fields and command numbers follow
[Dolphin's HID v4 implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/USB/USB_HID/HIDv4.cpp)
and the original MH3 SDK calls at `0x80528AF0` and `0x805279B0`.

The game's keyboard initialization spins at `0x8056C994` on a stack flag set by
its asynchronous completion callback. No host event service is called inside
that loop. A wrapper around the original HID initialization (`0x80528B90`) pumps
the real queued open/version/enumeration callbacks before returning. It preserves
the SDK's initialization logic and return value. This is a scoped delivery point;
general asynchronous preemption remains unfinished.

Runtime tests exercise version completion, initial enumeration, pending requests,
duplicate rejection, shutdown, close, absent-device errors and buffer validation.
The original tight wait was independently confirmed in `logs/hid-boot.sample.txt`.

The Metal boot in `logs/hid-ready.log` passes both HID and KBD initialization
after selecting Begin the Game. It proceeds into save-selection layout code,
where the next independent boundary is missing translated font target
`0x80507094`. Gameplay remains unverified. Both native and Metal runtime/audio
CTest suites pass.
