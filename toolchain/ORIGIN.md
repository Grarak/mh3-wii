# WiiCompiled-derived components

Translator and Launcher/WiiCompiled.Setup.Common copied from the WiiCompiled
project, commit `85f250155f8f397c1c463dc51dd4dc2707371d06`.
`../runtime/include` and the runtime's `abi_bridge.cpp`, `ppc_helpers.cpp`, and
`fpu_helpers.cpp` are also derived from that checkout. The GPLv3 license and
third-party notices are preserved here.

MH3-specific changes:

- Translator recursive discovery stops at full native replacements. Named
  `REGISTER_NATIVE_FUNCTION_AS` wrappers still discover their original bodies.
- Jump-table bounds follow word-scaled selector definitions instead of using
  stale comparisons of their destination register.
- MH3 ABI bridge removes Mario Kart's argument rewrite at `0x8023BD38` and its
  native CPU-call address catalog.
- Checked-memory FlatLoad/FlatStore also cover translator-proven stack accesses;
  MH3's initial memory backend does not reserve WiiCompiled's fixed host window.

The port retains static PowerPC-to-C++ translation and native host compilation.
There is no CPU interpreter or JIT fallback in this runtime.

The AX/DSP mixer that was adapted from the same WiiCompiled commit's
`runtime/src/hle/audio` directory has been removed; audio is now a native
mixer (`runtime/src/audio`).
