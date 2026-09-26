# WiiCompiled-derived components

Translator and Launcher/WiiCompiled.Setup.Common copied from the sibling
WiiCompiled checkout, commit `85f250155f8f397c1c463dc51dd4dc2707371d06`.
`../runtime/include` and the runtime's `abi_bridge.cpp`, `ppc_helpers.cpp`, and
`fpu_helpers.cpp` are also derived from that checkout. The GPLv3 license and
third-party notices are preserved here. Original source checkout is unchanged.

MH3-specific changes:

- Translator recursive discovery stops at full native replacements. Named
  `REGISTER_NATIVE_FUNCTION_AS` wrappers still discover their original bodies.
- MH3 ABI bridge removes Mario Kart's argument rewrite at `0x8023BD38` and its
  native CPU-call address catalog.
- Checked-memory FlatLoad/FlatStore also cover translator-proven stack accesses;
  MH3's initial memory backend does not reserve WiiCompiled's fixed host window.

The port retains static PowerPC-to-C++ translation and native host compilation.
There is no CPU interpreter or JIT fallback in this runtime.
