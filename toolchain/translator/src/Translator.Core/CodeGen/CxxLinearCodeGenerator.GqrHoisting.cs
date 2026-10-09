using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Translation;

namespace Translator.Core.CodeGen;

/// <summary>
/// Hoists GQR0-7 into locals read once per prologue instead of every psq_l/psq_st reload, re-reading
/// only after an mtspr to that GQR or a call not proven to leave it alone. Only the generic helpers
/// participate; stack forms have no GQR-value overload and <c>Known</c> forms template the value already.
/// </summary>
public sealed partial class CxxLinearCodeGenerator
{
    // Translation waves run Parallel.For over functions; ambient emitter state
    // is per-thread, exactly like the active residency.
    [ThreadStatic]
    private static IReadOnlySet<uint>? _hoistedGqrIndices;

    internal static string HoistedGqrName(long index) => $"mkw_gqr{index}";

    private static bool IsGqrHoisted(long index) =>
        _hoistedGqrIndices is { Count: > 0 } && _hoistedGqrIndices.Contains((uint)index);

    /// <summary>
    /// The GQR indices whose value a generic PSQ helper in this body would read
    /// out of the context. Guarded known sites count: their fallback arm is the
    /// generic helper.
    /// </summary>
    private static IReadOnlySet<uint> CollectHoistableGqrIndices(
        IrFunction function,
        StackAddressFacts stackFacts,
        bool enabled)
    {
        var indices = new HashSet<uint>();
        if (!enabled) return indices;

        foreach (var call in function.Blocks.SelectMany(static block => block.Instructions).OfType<IrCall>())
        {
            if (!TryGetGenericGqrPsqSite(call, out var index, out var addressArgument)) continue;
            // The stack forms resolve the host pointer through the frame cache
            // and have no GQR-value overload in the runtime.
            if (stackFacts.TryResolve(addressArgument, out _)) continue;
            indices.Add(index);
        }

        return indices;
    }

    /// <summary>
    /// True for the PSQ call shapes that emit a helper reading ctx-&gt;gqr[I]:
    /// the plain PPC_PsqL/PPC_PsqSt forms and the guarded known forms, whose
    /// else-arm is the plain form.
    /// </summary>
    private static bool TryGetGenericGqrPsqSite(IrCall call, out uint index, out IrValue addressArgument)
    {
        index = 0;
        addressArgument = null!;
        var isLoad = call.Target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase) ||
                     call.Target.StartsWith("PPC_PsqLKnownGuarded_", StringComparison.OrdinalIgnoreCase);
        var isStore = call.Target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase) ||
                      call.Target.StartsWith("PPC_PsqStKnownGuarded_", StringComparison.OrdinalIgnoreCase);
        if (!isLoad && !isStore) return false;
        var widthArgument = isLoad ? 1 : 2;
        var indexArgument = isLoad ? 2 : 3;
        if (call.Arguments.Count <= indexArgument) return false;
        if (call.Arguments[widthArgument].Constant is not (0 or 1)) return false;
        if (call.Arguments[indexArgument].Constant is not { } rawIndex || rawIndex is < 0 or > 7) return false;
        index = (uint)rawIndex;
        addressArgument = call.Arguments[0];
        return true;
    }

    private static void EmitHoistedGqrPrologue(
        StringBuilder body, IReadOnlySet<uint> hoistedGqrIndices)
    {
        if (hoistedGqrIndices.Count == 0) return;
        foreach (var index in hoistedGqrIndices.OrderBy(static value => value))
        {
            // maybe_unused: guarded-store sites become `if constexpr` arms under
            // _gqr_impl versioning, so one instantiation can discard every use.
            body.AppendLine($"    [[maybe_unused]] uint32_t {HoistedGqrName(index)} = ctx->gqr[{index}];");
        }
        body.AppendLine();
    }

    /// <summary>
    /// Re-reads hoisted locals after anything that can have written a GQR: an mtspr reloads just
    /// that register, direct guest calls use the interprocedural write mask, everything else reloads all.
    /// </summary>
    private static void EmitHoistedGqrReloads(
        StringBuilder body,
        string pad,
        IrInstruction instruction,
        IReadOnlySet<uint> hoistedGqrIndices,
        IReadOnlyDictionary<uint, byte>? gqrCalleeWriteMasks)
    {
        if (hoistedGqrIndices.Count == 0) return;

        IEnumerable<uint>? reloaded = null;
        switch (instruction)
        {
            case IrAssign assign when GqrConstantPropagation.TryGetGqrKey(assign.Destination) is { } assignKey:
                reloaded = new[] { (uint)(assignKey[3] - '0') };
                break;
            case IrPhi phi when GqrConstantPropagation.TryGetGqrKey(phi.Destination) is { } phiKey:
                reloaded = new[] { (uint)(phiKey[3] - '0') };
                break;
            case IrCall call when GqrConstantPropagation.TryGuestAddress(call.Target, out var target):
                {
                    var mask = gqrCalleeWriteMasks is not null &&
                               gqrCalleeWriteMasks.TryGetValue(target, out var knownMask)
                        ? knownMask
                        : (byte)0xFF;
                    if (mask == 0) return;
                    reloaded = hoistedGqrIndices.Where(index => (mask & (1 << (int)index)) != 0);
                    break;
                }
            case IrCall call:
                if (!HelperCallCanWriteGqrs(call)) return;
                reloaded = hoistedGqrIndices;
                break;
            case IrIndirectCall:
                reloaded = hoistedGqrIndices;
                break;
        }

        if (reloaded is null) return;
        foreach (var index in reloaded.Where(hoistedGqrIndices.Contains).OrderBy(static value => value))
        {
            body.AppendLine($"{pad}{HoistedGqrName(index)} = ctx->gqr[{index}];");
        }
    }

    /// <summary>
    /// Whether a non-guest helper call can leave a GQR stale. The catalog is authoritative; anything it
    /// doesn't model exactly reloads. One case it can't model: the generic SPR writer reaches GQR0-7
    /// through SPR 912-919, so it must not slip through as if it left them alone.
    /// </summary>
    private static bool HelperCallCanWriteGqrs(IrCall call)
    {
        if (call.Target.Equals("PPC_WriteSpr", StringComparison.OrdinalIgnoreCase))
        {
            return call.Arguments.Count == 0 ||
                   call.Arguments[0].Constant is not { } spr ||
                   spr is >= 912 and <= 919;
        }

        const GuestCallBoundaryFlags opaque =
            GuestCallBoundaryFlags.RequiresCompleteContext |
            GuestCallBoundaryFlags.CanSuspend |
            GuestCallBoundaryFlags.CanSwitchThreads |
            GuestCallBoundaryFlags.InvokesGuestCode;
        return (AnalyzeGuestHelperEffect(call).BoundaryFlags & opaque) != 0;
    }
}
