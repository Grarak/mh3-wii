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
/// def) and to the PowerPC EABI call boundary: a guest callee may clobber the volatile flags, so
/// the caller never reads its own across a call.
/// </summary>
public static class GuestFlagLivenessAnalyzer
{
    /// <summary>
    /// What a guest call clobbers under the EABI (see GuestAbiContractAnalyzer.IndirectCallContract):
    /// CR0, CR1, CR5-CR7 and XER.CA. It preserves CR2-CR4.
    /// </summary>
    public static GuestFlagState CallClobbered => new(0xE3, true);

    /// <summary>
    /// The caller's flags a guest callee may read: CR1, whose bit 6 tells a varargs function
    /// whether f1-f8 hold arguments, and CR2-CR4, which it preserves. Never XER.CA.
    /// </summary>
    private const byte CallReadableCrFields = 0x1E;

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

        if (instruction is IrCall helper && TryConditionHelperFlags(helper, out _, out var helperKills))
        {
            return helperKills;
        }

        if (IsGuestCall(instruction))
        {
            return CallClobbered;
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

            case IrPhi:
                // A phi only renames: the fields read after it are the ones its
                // sources must carry, and liveness here is per field, not per
                // SSA version, so it passes through unchanged.
                return GuestFlagState.None;

            case IrIndirectCall:
                // A call into unknown guest code, which still follows the EABI.
                return GuestFlagState.Cr(CallReadableCrFields);

            case IrIndirectJump:
                // A tail call, or a jump table the lifter couldn't resolve, into
                // code of this function that may read anything.
            case IrUndefined:
                return GuestFlagState.All;

            case IrCall call:
                return CallReads(call, context);

            case IrBinary { Op: "and", Left: { Kind: "register" } left, Right.Constant: long mask }
                when RegisterFlags(left.RegisterName).CrFields == byte.MaxValue:
                // mtcrf's copy of the fields it keeps: the others are masked off.
                return GuestFlagState.Cr(FieldsOfMask(unchecked((uint)mask)));

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
            if (GuestAbiContractAnalyzer.IsSaveRestoreThunk(target))
            {
                return GuestFlagState.None;
            }

            // A target without a usable contract (unknown, or one a mod may replace)
            // still follows the EABI.
            if (context.OpaqueCallTargets.Contains(target) ||
                !context.CalleeContracts.TryGetValue(target, out var contract))
            {
                return GuestFlagState.Cr(CallReadableCrFields);
            }

            return contract.HasFullSynchronizationFence
                ? GuestFlagState.All
                : GuestFlagState.Cr((byte)(contract.CrReadBeforeWriteMask & CallReadableCrFields));
        }

        if (TryConditionHelperFlags(call, out var helperReads, out _))
        {
            return helperReads;
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
    /// The flags a CR helper reads and the fields it overwrites whole. The catalog has them read
    /// all eight, since each rewrites the packed CR and the register cache needs it complete; but
    /// the others pass through unchanged, which is no read. False for anything else, or a field
    /// that isn't a constant.
    /// </summary>
    private static bool TryConditionHelperFlags(IrCall call, out GuestFlagState reads, out GuestFlagState kills)
    {
        reads = GuestFlagState.None;
        kills = GuestFlagState.None;

        int Field(int argument) =>
            call.Arguments.Count > argument && call.Arguments[argument].Constant is long value ? (int)(value & 31) : -1;

        switch (call.Target)
        {
            case "PPC_Fcmp":
            case "PPC_PsCmpo0":
            case "PPC_PsCmpo1":
            case "PPC_PsCmpu0":
            case "PPC_PsCmpu1":
            case "PPC_Mcrfs":
                if (Field(0) < 0) return false;
                kills = GuestFlagState.CrField(Field(0) & 7);
                return true;

            case "PPC_Mcrxr":
                // Copies SO, OV and CA into the field and clears them.
                if (Field(0) < 0) return false;
                reads = GuestFlagState.CarryOnly;
                kills = GuestFlagState.CrField(Field(0) & 7);
                return true;

            case "PPC_Mcrf":
                if (Field(0) < 0 || Field(1) < 0) return false;
                reads = GuestFlagState.CrField(Field(1) & 7);
                kills = GuestFlagState.CrField(Field(0) & 7);
                return true;

            case "PPC_CrSetBit":
                // One bit: the rest of its field is kept.
                if (Field(0) < 0) return false;
                reads = GuestFlagState.CrField(Field(0) / 4);
                return true;

            case "PPC_CrLogical":
                // Target, left and right bits; the target's field keeps its other bits.
                if (Field(1) < 0 || Field(2) < 0 || Field(3) < 0) return false;
                reads = GuestFlagState.CrField(Field(1) / 4)
                    .Union(GuestFlagState.CrField(Field(2) / 4))
                    .Union(GuestFlagState.CrField(Field(3) / 4));
                return true;

            default:
                return false;
        }
    }

    /// <summary>The CR fields with any bit set in a packed-CR mask (CR0 is the top nibble).</summary>
    private static byte FieldsOfMask(uint mask)
    {
        byte fields = 0;

        for (var field = 0; field < 8; ++field)
        {
            if (((mask >> (28 - 4 * field)) & 0xFu) != 0)
                fields |= (byte)(1 << field);
        }

        return fields;
    }

    /// <summary>
    /// A call into guest code: a direct call to a guest address other than a save/restore thunk,
    /// or an indirect one.
    /// </summary>
    public static bool IsGuestCall(IrInstruction instruction) =>
        instruction is IrIndirectCall ||
        (instruction is IrCall call && GuestTargetParser.TryParseAddress(call.Target, out var target) &&
         !GuestAbiContractAnalyzer.IsSaveRestoreThunk(target));

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
