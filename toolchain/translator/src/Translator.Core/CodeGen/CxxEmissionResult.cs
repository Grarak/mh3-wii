using System.Collections.Generic;
using Translator.Core.Analysis;

namespace Translator.Core.CodeGen;

/// <summary>
/// The facts one emitted state-free interface published, mirroring the <c>// RECOMP_STATE_FREE_ABI ...</c>
/// persistence comment so this record avoids re-parsing that text in the same process invocation.
/// <see cref="GprInputMask"/> is the native GPR parameter mask, derived from the contract rather than stored on it.
/// </summary>
public sealed record GuestStateFreeEmissionFacts(
    uint EntryPoint,
    string Symbol,
    GuestAbiContract Contract,
    uint GprInputMask);

/// <summary>
/// Build registration facts for one emitted function. Present only for the base
/// (non-mod) registration form.
/// </summary>
public sealed record CxxBuildRegistrationFacts(
    string Symbol,
    bool PreservesNonvolatileFprs,
    uint NonvolatileFprWriteMask);

/// <summary>
/// Structured result of emitting one function: the C++ text plus every fact the emitter derived, for same-process
/// consumers. The comment markers in <see cref="Code"/> remain the cross-run persistence format for build sharding and dispatch profiles.
/// </summary>
public sealed record CxxEmissionResult(
    string Code,
    GuestAbiContract GuestAbiContract,
    string GuestAbiMarker,
    CxxBuildRegistrationFacts? Registration,
    IReadOnlyList<uint> EmittedDirectCallTargets,
    IReadOnlyList<uint> EmittedLocalLabelAddresses,
    GuestStateFreeEmissionFacts? StateFree,
    IReadOnlyList<GuestStateFreeEmissionFacts> StateFreeEntryVariants);
