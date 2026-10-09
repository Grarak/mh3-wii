# PowerPC floating point on the Cortex-A9

## What the original does

The Wii's Broadway CPU (a Gekko derivative) has:

- fused multiply-add instructions (`fmadd`, `fmadds`, `ps_madd`, ...) that
  round once;
- paired singles: two float lanes in one FPR, with their own NaN and
  rounding behaviour;
- `fmuls`/`fmadds` that round the second operand to a 25-bit significand;
- FPSCR[NI], a non-IEEE mode that flushes subnormal results to zero. MH3
  runs with NI set.

The translator's ISA headers (`runtime/include/isa/ppc_isa_float.h`, from
WiiCompiled) reproduce all of this exactly on hosts with a fused
multiply-add (AArch64, x86-64 with FMA).

## What the port does instead

The Cortex-A9 (VFPv3, NEON v1) has no fused multiply-add, and its NEON unit
always flushes subnormals and returns the default NaN. The port keeps the
semantics that matter and gives up bit-exactness where it costs too much:

| Item | arm32 behaviour | Exact form |
| --- | --- | --- |
| Guest fused multiply-add | separate multiply and add, rounding twice (`PpcFmaDoubleInline`, `PpcFmaFloatInline`) | `-DMH3_EXACT_FMA`: libm `fma` for doubles, a double-precision detour with a tie fix-up for floats |
| Native C math (`mh3_madd`) | `a*b + c` (`vmla.f32` or `vmul` + `vadd`) instead of glibc's software `fmaf` | fused on hosts that have it |
| FPSCR[NI] | host FZ is set exactly when NI is; FZ flushes on the unrounded exponent, which is NI's rule, so no software mask | same on AArch64 |
| Paired singles | two VFP scalars, not NEON (NEON ignores FPSCR) | |
| NaN check of paired results | one `vcmp` of the two lanes, the rewrite out of line | |
| `fmuls` 25-bit rounding | skipped where the compiler can see the operand is already a single | |
| `stfs` of a double | an exact single (an `lfs` result or a single-precision op) is stored as its float bits; otherwise the narrowing runs on integer halves, with no VFP self-compare | |

The first two rows change results (by rounding); the others are exact. The
64-bit Linux build keeps the exact fused forms; the arm32 builds (the Pi and
the Vita) use the two-rounding multiply-add, and so does the web build:
wasm32 takes the arm32 paths of the ISA headers. There the FP control
register is a software word, and WebAssembly always keeps denormals, so
FPSCR[NI] flushes nothing ([web build](../platforms/web-build.md)).

## Why

Measured on the game thread (commit messages and the header comment):

- Exact fused results on the A9: about 0.56 ms of the Pi 5 game thread per
  frame ("Make separate multiply-add the default for guest fused ops on the
  Cortex-A9"). In play the two-rounding results showed no difference in
  movement, collision, audio or frames beyond 1-level pixel differences.
- Software `fmaf` in the native C code: 1.8% of the Pi game thread in
  Alatreon movement ("Use a plain multiply-add instead of software fmaf on
  the Cortex-A9").
- `stfs` narrowing: the store tested `(double)(float)v == v`, which the
  compiler cannot fold for a widened float (a NaN fails it), so every such
  store kept a VFP compare and a flags move. The test now compares bit
  patterns under `__builtin_constant_p`, which folds for a widened float:
  VFP self-compares in the Vita binary 19,546 → 1,404, `vmrs APSR`
  39,762 → 20,387. Bit-identical on 24.5 M doubles and every float
  pattern, except that a signaling NaN is stored as is, as `lfs`/`stfs` do
  on hardware. With the [flag elision](flag-elision.md) it took the Vita's
  Jhen Mohran logic from 12.48 to 11.48 ms a frame.
- The NI mask on every single-precision result: arm64 4.74 → 4.44 ms per frame,
  Pi 5 quest about 51 → 54 fps ("Let host flush-to-zero apply FPSCR[NI] on
  ARM without a mask"); on AArch32 it was a VFP compare whose flags moved to
  the core, which stalls the A9.

## Example

`runtime/include/isa/ppc_isa_float.h`:

```cpp
#if defined(MH3_EXACT_FMA) || !defined(__arm__) || defined(__ARM_FEATURE_FMA)
#define MH3_FUSED_MADD 1
#endif

inline double PpcFmaDoubleInline(double a, double b, double c) {
#if defined(MH3_FUSED_MADD)
    return std::fma(a, b, c);
#else
    return a * b + c;
#endif
}
```

## Applying it to another Wii game

Generic: every Wii game uses fused multiply-adds and paired singles through
the SDK's matrix library and the compiler. The trade-off is the same for
any game on a host without fused multiply-add: exact emulation is possible
(`MH3_EXACT_FMA`) but costs time on every matrix operation.

Game-specific: whether the two-rounding results matter. Collision, physics
that accumulate over frames, and code that compares floats for equality
are where a difference would show. Run the game with and without
`MH3_EXACT_FMA`, compare the probes this project uses (port checks at a
fixed step, frame dumps, the step probe), and play the cases that depend on
precise positions. MH3 showed no visible difference; another game may.
