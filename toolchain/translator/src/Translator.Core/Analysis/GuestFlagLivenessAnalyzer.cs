using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// The eight CR fields and the XER carry bit, tracked separately from <see cref="GuestStateMask"/> for
/// sub-register granularity. Compare and carry read/modify/write helpers touch XER only to preserve
/// bits they don't own, so that must not count as a carry read or every carry update would look live.
/// </summary>
public readonly record struct GuestFlagState(byte CrFields, bool Carry)
{
    public static GuestFlagState None => default;

    /// <summary>Everything live. The conservative answer for any unknown boundary.</summary>
    public static GuestFlagState All => new(byte.MaxValue, true);

    public GuestFlagState Union(GuestFlagState other) =>
        new((byte)(CrFields | other.CrFields), Carry || other.Carry);

    public GuestFlagState Except(GuestFlagState other) =>
        new((byte)(CrFields & ~other.CrFields), Carry && !other.Carry);

    public bool HasCrField(int field) => (CrFields & (1 << field)) != 0;

    public static GuestFlagState Cr(byte fields) => new(fields, false);

    public static GuestFlagState CrField(int field) => new((byte)(1 << field), false);

    public static GuestFlagState CarryOnly => new(0, true);
}

/// <summary>
/// What the flag liveness transfer needs at a call site. A target missing from
/// <see cref="CalleeContracts"/>, or listed in <see cref="OpaqueCallTargets"/> (a mod may replace
/// it, so the published contract may not match what actually runs), is a complete flag boundary.
/// </summary>
public sealed record GuestFlagLivenessContext(
    IReadOnlyDictionary<uint, GuestAbiContract> CalleeContracts,
    IReadOnlySet<uint> OpaqueCallTargets)
{
    public static GuestFlagLivenessContext Empty { get; } = new(
        new Dictionary<uint, GuestAbiContract>(), new HashSet<uint>());
}

public sealed record GuestFlagLivenessResult(
    IReadOnlyDictionary<string, GuestFlagState> LiveIn,
    IReadOnlyDictionary<string, GuestFlagState> LiveOut);

/// <summary>
/// Backward per-CR-field and XER.CA liveness over the emitted IR. Reads are collected aggressively
/// while kills are limited to provably total writes (<see cref="IrSetCrField"/>, a full <c>xer</c>
/// def), so a call never kills and a callee that may rewrite a flag can't erase a caller-side update.
/// </summary>
public static class GuestFlagLivenessAnalyzer
{
    /// <summary>
    /// Helpers whose only carry effect is overwriting XER.CA; the surrounding-bit read to
    /// preserve them doesn't count as a carry read.
    /// </summary>
    private static readonly HashSet<string> CarryProducingHelpers = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_UpdateCarryAdd", "PPC_UpdateCarrySub", "PPC_UpdateCarryShiftRight"
    };

    private static readonly Regex GetCrBitRegex = new(
        @"GetCRBit\(\s*ctx\s*,\s*(?<field>\d+)\s*,\s*(?<bit>\d+)\s*\)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    public static bool IsCarryProducingHelper(string target) => CarryProducingHelpers.Contains(target);

    public static GuestFlagLivenessResult Analyze(
        IrFunction function,
        IrCfg cfg,
        GuestFlagLivenessContext context,
        GuestFlagState exitLive)
    {
        var liveIn = function.Blocks.ToDictionary(
            block => block.Label, _ => GuestFlagState.None, StringComparer.OrdinalIgnoreCase);
        var liveOut = function.Blocks.ToDictionary(
            block => block.Label, _ => GuestFlagState.None, StringComparer.OrdinalIgnoreCase);

        bool changed;
        do
        {
            changed = false;
            for (var blockIndex = function.Blocks.Count - 1; blockIndex >= 0; --blockIndex)
            {
                var block = function.Blocks[blockIndex];
                var successors = cfg.Successors(block.Label);
                var outgoing = exitLive;
                if (successors.Count != 0)
                {
                    outgoing = GuestFlagState.None;
                    for (var successorIndex = 0; successorIndex < successors.Count; ++successorIndex)
                    {
                        outgoing = outgoing.Union(
                            liveIn.TryGetValue(successors[successorIndex], out var successorIn)
                                ? successorIn
                                : exitLive);
                    }
                }

                var incoming = outgoing;
                for (var index = block.Instructions.Count - 1; index >= 0; --index)
                {
                    incoming = Transfer(block.Instructions[index], incoming, context);
                }

                if (outgoing != liveOut[block.Label] || incoming != liveIn[block.Label])
                {
                    liveOut[block.Label] = outgoing;
                    liveIn[block.Label] = incoming;
                    changed = true;
                }
            }
        } while (changed);

        return new GuestFlagLivenessResult(liveIn, liveOut);
    }

    /// <summary>
    /// Backward transfer for one instruction: <c>(live \ kills) ∪ reads</c>.
    /// </summary>
    public static GuestFlagState Transfer(
        IrInstruction instruction,
        GuestFlagState live,
        GuestFlagLivenessContext context) =>
        live.Except(Kills(instruction)).Union(Reads(instruction, context));

    public static GuestFlagState Kills(IrInstruction instruction)
    {
        if (instruction is IrSetCrField setCr)
        {
            return GuestFlagState.CrField(setCr.FieldIndex & 7);
        }

        // A definition of the whole XER register replaces the carry bit. That
        // covers mtxer/mtspr as well as the carry read/modify/write helpers,
        // whose IR destination is xer.
        foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
        {
            if (RegisterNameUtils.HardwareBase(definition.AsSpan()).Equals("xer", StringComparison.OrdinalIgnoreCase))
            {
                return GuestFlagState.CarryOnly;
            }
        }

        return GuestFlagState.None;
    }

    public static GuestFlagState Reads(IrInstruction instruction, GuestFlagLivenessContext context)
    {
        switch (instruction)
        {
            case IrSetCrField:
                // Reads XER.SO only, and no CR field: the compare overwrites the
                // complete field from its two operands.
                return GuestFlagState.None;

            case IrBranch branch:
                return GuestFlagState.Cr(BranchCrFields(branch));

            case IrIndirectCall:
            case IrIndirectJump:
            case IrUndefined:
                return GuestFlagState.All;

            case IrCall call:
                return CallReads(call, context);

            default:
                {
                    var result = GuestFlagState.None;
                    foreach (var use in IrRegisterDataFlow.Uses(instruction))
                    {
                        result = result.Union(RegisterFlags(use));
                    }
                    return result;
                }
        }
    }

    private static GuestFlagState CallReads(IrCall call, GuestFlagLivenessContext context)
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var target))
        {
            if (context.OpaqueCallTargets.Contains(target) ||
                !context.CalleeContracts.TryGetValue(target, out var contract))
            {
                return GuestFlagState.All;
            }

            return contract.HasFullSynchronizationFence
                ? GuestFlagState.All
                : new GuestFlagState(contract.CrReadBeforeWriteMask, contract.ReadsXerBeforeWrite);
        }

        // Non-address targets are either runtime helpers with a cataloged
        // effect, or guest symbols the emitter calls directly. The catalog
        // classifies anything it does not recognize as a complete boundary.
        var effect = GuestHelperEffectCatalog.Analyze(call);
        var readsCarry = effect.ReadsXer && !CarryProducingHelpers.Contains(call.Target);
        var result = new GuestFlagState(effect.CrReadMask, readsCarry);
        foreach (var argument in call.Arguments)
        {
            if (argument.Kind == "register" && argument.RegisterName is { } name)
            {
                result = result.Union(RegisterFlags(name));
            }
        }

        return result;
    }

    /// <summary>
    /// CR fields a branch condition observes. The lifter emits general <c>bc</c>/<c>bcctr</c> forms
    /// as raw C++ text rather than a register operand, so the text must be inspected; anything that
    /// can't be parsed conservatively reports every field.
    /// </summary>
    public static byte BranchCrFields(IrBranch branch)
    {
        var conditionRegister = branch.ConditionRegister;
        if (string.IsNullOrEmpty(conditionRegister))
        {
            return 0;
        }

        if (conditionRegister.Contains("GetCRBit", StringComparison.Ordinal) ||
            conditionRegister.Contains("ctx->cr", StringComparison.Ordinal) ||
            conditionRegister.Contains("cached_cr", StringComparison.Ordinal))
        {
            if (conditionRegister.Contains("ctx->cr", StringComparison.Ordinal) ||
                conditionRegister.Contains("cached_cr", StringComparison.Ordinal))
            {
                return byte.MaxValue;
            }

            byte mask = 0;
            var matches = GetCrBitRegex.Matches(conditionRegister);
            if (matches.Count != CountOccurrences(conditionRegister, "GetCRBit"))
            {
                return byte.MaxValue;
            }

            foreach (Match match in matches)
            {
                mask |= (byte)(1 << (int.Parse(match.Groups["field"].Value) & 7));
            }

            return mask;
        }

        return RegisterFlags(conditionRegister).CrFields;
    }

    private static int CountOccurrences(string text, string value)
    {
        var count = 0;
        var index = text.IndexOf(value, StringComparison.Ordinal);
        while (index >= 0)
        {
            ++count;
            index = text.IndexOf(value, index + value.Length, StringComparison.Ordinal);
        }

        return count;
    }

    private static GuestFlagState RegisterFlags(string? name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return GuestFlagState.None;
        }

        var baseName = RegisterNameUtils.HardwareBase(name.AsSpan());
        if (baseName.Length == 3 && baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
            baseName[2] is >= '0' and <= '7')
        {
            return GuestFlagState.CrField(baseName[2] - '0');
        }

        if (baseName.Length is >= 4 and <= 5 && baseName.StartsWith("crb", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(baseName[3..], out var crBit) && crBit is >= 0 and < 32)
        {
            return GuestFlagState.CrField(crBit / 4);
        }

        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase))
        {
            return GuestFlagState.Cr(byte.MaxValue);
        }

        if (baseName.Equals("xer", StringComparison.OrdinalIgnoreCase))
        {
            return GuestFlagState.CarryOnly;
        }

        return GuestFlagState.None;
    }
}
