using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    internal enum FusedComparison
    {
        Equal,
        NotEqual,
        Less,
        GreaterOrEqual,
        Greater,
        LessOrEqual
    }

    /// <summary>
    /// A compare whose only consumer is the branch that terminates the same
    /// block. The branch emits the C++ comparison directly and the CR field is
    /// never materialized.
    /// </summary>
    internal sealed record FusedCompare(
        IrValue Left,
        IrValue Right,
        bool IsUnsigned,
        bool IsFloat,
        FusedComparison Comparison);

    /// <summary>
    /// Everything the body emitter needs to drop condition-flag work: the
    /// instructions it must not emit at all, and the branches that replace a
    /// packed CR test with a direct comparison.
    /// </summary>
    internal sealed record FlagElisionPlan(
        IReadOnlySet<(string Block, int Index)> SuppressedInstructions,
        IReadOnlyDictionary<string, FusedCompare> FusedBranches)
    {
        public static FlagElisionPlan Empty { get; } = new(
            new HashSet<(string, int)>(),
            new Dictionary<string, FusedCompare>(StringComparer.OrdinalIgnoreCase));

    }

    /// <summary>
    /// Materializes <see cref="FlagElisionPlan.SuppressedInstructions"/> as one dense flag array per block,
    /// replacing a per-emission <c>(string, int)</c> tuple hash lookup for every instruction. Block keys use
    /// ordinal comparison to match the set's tuple comparer.
    /// </summary>
    private static Dictionary<string, bool[]> BuildSuppressedInstructionMasks(
        IrFunction function,
        FlagElisionPlan plan)
    {
        var masks = new Dictionary<string, bool[]>(StringComparer.Ordinal);
        if (plan.SuppressedInstructions.Count == 0)
        {
            return masks;
        }

        var blockSizes = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var block in function.Blocks)
        {
            blockSizes[block.Label] = block.Instructions.Count;
        }

        foreach (var (blockLabel, index) in plan.SuppressedInstructions)
        {
            if (index < 0)
            {
                continue;
            }

            if (!masks.TryGetValue(blockLabel, out var mask))
            {
                var size = blockSizes.TryGetValue(blockLabel, out var known) ? known : index + 1;
                mask = new bool[Math.Max(size, index + 1)];
                masks[blockLabel] = mask;
            }
            else if (index >= mask.Length)
            {
                Array.Resize(ref mask, index + 1);
                masks[blockLabel] = mask;
            }

            mask[index] = true;
        }

        return masks;
    }

    /// <summary>
    /// Condition-flag elision planner: one backward liveness pass drives compare+branch fusion, dead CR-field
    /// writes, and dead XER.CA updates. Exit state is <see cref="GuestFlagState.All"/> since a return publishes
    /// ctx-&gt;cr/ctx-&gt;xer with no caller info, so the last write on any path to a return is always kept.
    /// </summary>
    private static FlagElisionPlan PlanFlagElision(
        IrFunction function,
        IrCfg cfg,
        RepresentationEnvironment types,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets)
    {
        // A body that dispatches on ctx->lr can re-enter any of its own labels,
        // including positions inside a block. The static CFG does not model
        // those edges, so no liveness statement derived from it holds and the
        // whole plan is abandoned for such functions.
        if (HasOutOfBandLocalEntry(function, nonReturningCallTargets, lrContinuationCallTargets))
        {
            return FlagElisionPlan.Empty;
        }

        var suppressed = new HashSet<(string Block, int Index)>();
        var fused = new Dictionary<string, FusedCompare>(StringComparer.OrdinalIgnoreCase);

        {
            var context = new GuestFlagLivenessContext(guestAbiContracts, modOverridableCallTargets);
            var liveness = GuestFlagLivenessAnalyzer.Analyze(function, cfg, context, GuestFlagState.All);
            var fusedCompareSites = new HashSet<(string Block, int Index)>();

            {
                foreach (var block in function.Blocks)
                {
                    if (!TryPlanCompareBranchFusion(
                            function, cfg, block, liveness.LiveOut[block.Label], types,
                            out var compare, out var compareBlock, out var compareIndex))
                    {
                        continue;
                    }

                    fused[block.Label] = compare!;
                    fusedCompareSites.Add((compareBlock!, compareIndex));
                    suppressed.Add((compareBlock!, compareIndex));
                }
            }

            {
                foreach (var block in function.Blocks)
                {
                    var live = liveness.LiveOut[block.Label];
                    for (var index = block.Instructions.Count - 1; index >= 0; --index)
                    {
                        var instruction = block.Instructions[index];
                        if (fusedCompareSites.Contains((block.Label, index)))
                        {
                            // Already removed, but the branch that replaced it
                            // still consumes the value: it kills the field for
                            // everything above, exactly as the compare did.
                            live = live.Except(GuestFlagLivenessAnalyzer.Kills(instruction));
                            continue;
                        }

                        switch (instruction)
                        {
                            case IrSetCrField setCr when !live.HasCrField(setCr.FieldIndex & 7):
                                suppressed.Add((block.Label, index));
                                continue;

                            case IrCall call when !live.Carry &&
                                                  GuestFlagLivenessAnalyzer.IsCarryProducingHelper(call.Target) &&
                                                  IsXerDestination(call.Destination):
                                suppressed.Add((block.Label, index));
                                continue;
                        }

                        live = GuestFlagLivenessAnalyzer.Transfer(instruction, live, context);
                    }
                }
            }
        }

        {
            foreach (var block in function.Blocks)
            {
                for (var index = 0; index + 1 < block.Instructions.Count; ++index)
                {
                    if (block.Instructions[index] is not IrAssign { Value.Kind: "const" } assign ||
                        !GetRegisterBaseName(assign.Destination).Equals("lr", StringComparison.OrdinalIgnoreCase) ||
                        block.Instructions[index + 1] is not IrCall call ||
                        !CalleeIgnoresLinkRegister(
                            call, guestAbiContracts, modOverridableCallTargets,
                            nonReturningCallTargets, lrContinuationCallTargets))
                    {
                        continue;
                    }

                    suppressed.Add((block.Label, index));
                }
            }
        }

        if (suppressed.Count == 0 && fused.Count == 0)
        {
            return FlagElisionPlan.Empty;
        }

        return new FlagElisionPlan(suppressed, fused);
    }

    /// <summary>
    /// True when the body has a call whose callee may resume it via <c>EmitLocalLrContinuationDispatch</c>,
    /// which can jump to any <c>loc_</c> label including ones inside a block, so the static CFG understates edges.
    /// </summary>
    private static bool HasOutOfBandLocalEntry(
        IrFunction function,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets)
    {
        if (nonReturningCallTargets.Count == 0 && lrContinuationCallTargets.Count == 0)
        {
            return false;
        }

        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is IrCall call &&
                    TryParseAddress(call.Target, out var target) &&
                    (nonReturningCallTargets.Contains(target) || lrContinuationCallTargets.Contains(target)))
                {
                    return true;
                }
            }
        }

        return false;
    }

    private static bool IsXerDestination(string? destination) =>
        !string.IsNullOrWhiteSpace(destination) &&
        GetRegisterBaseName(destination!).Equals("xer", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// A call whose return address never becomes observable state: translated functions return via the C++
    /// stack, not by branching to ctx-&gt;lr, so the LR store is skipped unless the callee reads LR, the call site
    /// dispatches on ctx-&gt;lr, or the callee escapes the contract model entirely.
    /// </summary>
    private static bool CalleeIgnoresLinkRegister(
        IrCall call,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets)
    {
        if (!TryParseAddress(call.Target, out var target) ||
            modOverridableCallTargets.Contains(target) ||
            nonReturningCallTargets.Contains(target) ||
            lrContinuationCallTargets.Contains(target) ||
            !guestAbiContracts.TryGetValue(target, out var contract))
        {
            return false;
        }

        const GuestCallBoundaryFlags escaping =
            GuestCallBoundaryFlags.RequiresCompleteContext |
            GuestCallBoundaryFlags.CanSuspend |
            GuestCallBoundaryFlags.CanSwitchThreads |
            GuestCallBoundaryFlags.InvokesGuestCode;

        return !contract.ReadsLrBeforeWrite && (contract.BoundaryFlags & escaping) == 0;
    }

    /// <summary>
    /// How many basic blocks back the compare feeding a branch is searched. Compare/branch pairs are adjacent
    /// in PowerPC, but block builders often split a new block at the branch, so the producer is usually at the
    /// end of the single predecessor.
    /// </summary>
    private const int MaxFusionBlockHops = 4;

    /// <summary>
    /// Finds the compare feeding a block's terminating branch when the branch is its only consumer, searching
    /// back through a chain of blocks with exactly one predecessor/successor each and no conditional transfer.
    /// </summary>
    private static bool TryPlanCompareBranchFusion(
        IrFunction function,
        IrCfg cfg,
        IrBasicBlock block,
        GuestFlagState liveOut,
        RepresentationEnvironment types,
        out FusedCompare? compare,
        out string? compareBlock,
        out int compareIndex)
    {
        compare = null;
        compareBlock = null;
        compareIndex = -1;
        if (block.Instructions.Count == 0 ||
            block.Instructions[^1] is not IrBranch branch ||
            !TryParseSingleCrBitTest(branch, out var field, out var bit, out var testSet))
        {
            return false;
        }

        // The summary-overflow copy is not reconstructible from the compare
        // operands, so a branch on it can never be fused.
        if (bit == CrBit.SummaryOverflow || liveOut.HasCrField(field))
        {
            return false;
        }

        // Instructions executed between the compare and the branch. The fused
        // comparison is evaluated at the branch, so none of them may redefine an
        // operand.
        var between = new List<IrInstruction>();
        var current = block;
        var startIndex = block.Instructions.Count - 2;
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { block.Label };

        for (var hop = 0; hop < MaxFusionBlockHops; ++hop)
        {
            for (var index = startIndex; index >= 0; --index)
            {
                var instruction = current.Instructions[index];
                if (instruction is IrSetCrField setCr && (setCr.FieldIndex & 7) == field)
                {
                    if (between.Any(later => RedefinesFusedOperand(later, setCr)))
                    {
                        return false;
                    }

                    var candidate = BuildFusedCompare(setCr, bit, testSet, types);
                    // Paired-single representation is normalized on CFG edges by
                    // emitted code that no IR definition describes, so a float
                    // compare is only re-evaluated inside its own block.
                    if (candidate is null || (candidate.IsFloat && hop != 0))
                    {
                        return false;
                    }

                    compare = candidate;
                    compareBlock = current.Label;
                    compareIndex = index;
                    return true;
                }

                if (!IsFusionTransparent(instruction, field))
                {
                    return false;
                }

                between.Add(instruction);
            }

            // The function entry is always reachable without executing any
            // predecessor, so a single recorded predecessor proves nothing there.
            if (current.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            var predecessors = cfg.Predecessors(current.Label);
            if (predecessors.Count != 1)
            {
                return false;
            }

            var predecessorLabel = predecessors[0];
            if (!visited.Add(predecessorLabel) ||
                cfg.Successors(predecessorLabel).Count != 1 ||
                !cfg.Blocks.TryGetValue(predecessorLabel, out var predecessor) ||
                predecessor.Instructions.Count == 0)
            {
                return false;
            }

            // Only a plain fallthrough or unconditional goto may sit between the
            // compare and the branch.
            var terminator = predecessor.Instructions[^1];
            if (terminator is IrBranch or IrJumpTable or IrIndirectJump or IrReturn or IrUndefined)
            {
                return false;
            }

            current = predecessor;
            startIndex = terminator is IrJump
                ? predecessor.Instructions.Count - 2
                : predecessor.Instructions.Count - 1;
        }

        return false;
    }

    private enum CrBit
    {
        LessThan,
        GreaterThan,
        Equal,
        SummaryOverflow
    }

    /// <summary>
    /// An instruction that may sit between a compare and the branch it feeds. The fused comparison is evaluated
    /// at the branch, so nothing between them may redefine an operand or observe the CR field; calls are
    /// excluded outright since they can do both.
    /// </summary>
    private static bool IsFusionTransparent(IrInstruction instruction, int field)
    {
        switch (instruction)
        {
            case IrTracePpc:
            case IrComment:
                return true;
            // A compare into a different field neither reads CR nor touches the
            // field being fused; the matching field is resolved by the caller.
            case IrSetCrField other:
                return (other.FieldIndex & 7) != field;
            case IrCall:
            case IrIndirectCall:
            case IrIndirectJump:
            case IrJumpTable:
            case IrUndefined:
            case IrPhi:
                return false;
        }

        foreach (var name in IrRegisterDataFlow.Definitions(instruction)
                     .Concat(IrRegisterDataFlow.Uses(instruction)))
        {
            var baseName = RegisterNameUtils.HardwareBase(name);
            if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase) ||
                (baseName.Length == 3 && baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
                 baseName[2] is >= '0' and <= '7' && baseName[2] - '0' == field) ||
                baseName.StartsWith("crb", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// The operand registers a fused comparison re-reads at the branch. Any
    /// redefinition between the compare and the branch makes the fusion unsound.
    /// </summary>
    private static bool RedefinesFusedOperand(IrInstruction instruction, IrSetCrField compare)
    {
        foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
        {
            var baseName = RegisterNameUtils.HardwareBase(definition);
            if (MatchesOperand(compare.Left, baseName) || MatchesOperand(compare.Right, baseName))
            {
                return true;
            }
        }

        return false;

        static bool MatchesOperand(IrValue value, string baseName) =>
            value.Kind == "register" && value.RegisterName is { } name &&
            RegisterNameUtils.HardwareBase(name).Equals(baseName, StringComparison.OrdinalIgnoreCase);
    }

    private static FusedCompare? BuildFusedCompare(
        IrSetCrField setCr,
        CrBit bit,
        bool testSet,
        RepresentationEnvironment types)
    {
        var comparison = (bit, testSet) switch
        {
            (CrBit.Equal, true) => FusedComparison.Equal,
            (CrBit.Equal, false) => FusedComparison.NotEqual,
            (CrBit.LessThan, true) => FusedComparison.Less,
            (CrBit.LessThan, false) => FusedComparison.GreaterOrEqual,
            (CrBit.GreaterThan, true) => FusedComparison.Greater,
            (CrBit.GreaterThan, false) => FusedComparison.LessOrEqual,
            _ => (FusedComparison?)null
        };

        if (comparison is null)
        {
            return null;
        }

        var isFloat = IsFloatValue(setCr.Left, types) || IsFloatValue(setCr.Right, types);
        return new FusedCompare(setCr.Left, setCr.Right, setCr.IsUnsigned, isFloat, comparison.Value);
    }

    /// <summary>
    /// Decodes the single CR bit a conditional branch tests. Returns false for
    /// counter-based forms, for multi-bit raw expressions, and for anything the
    /// lifter produced that this decoder does not recognize exactly.
    /// </summary>
    private static bool TryParseSingleCrBitTest(IrBranch branch, out int field, out CrBit bit, out bool testSet)
    {
        field = 0;
        bit = CrBit.SummaryOverflow;
        testSet = false;

        if (TryParseRawCrBitTest(branch.ConditionRegister, out field, out var rawBit, out testSet))
        {
            bit = rawBit;
            return true;
        }

        var conditionBase = BaseRegister(branch.ConditionRegister ?? string.Empty);
        if (conditionBase.Length != 3 ||
            !conditionBase.StartsWith("cr", StringComparison.OrdinalIgnoreCase) ||
            conditionBase[2] is < '0' or > '7')
        {
            return false;
        }

        field = conditionBase[2] - '0';
        (bit, testSet) = branch.Condition.ToLowerInvariant() switch
        {
            "beq" => (CrBit.Equal, true),
            "bne" => (CrBit.Equal, false),
            "bgt" => (CrBit.GreaterThan, true),
            "ble" => (CrBit.GreaterThan, false),
            "blt" => (CrBit.LessThan, true),
            "bge" => (CrBit.LessThan, false),
            _ => (CrBit.SummaryOverflow, false)
        };

        return bit != CrBit.SummaryOverflow;
    }

    /// <summary>
    /// Matches the exact text <c>PpcLifter.BuildBoConditionExpression</c> emits
    /// for a <c>bc</c> form that tests one CR bit and ignores CTR. Anything else
    /// - including a CTR-decrementing form - is rejected rather than guessed at.
    /// </summary>
    private static bool TryParseRawCrBitTest(string? conditionText, out int field, out CrBit bit, out bool testSet)
    {
        field = 0;
        bit = CrBit.SummaryOverflow;
        testSet = false;
        if (string.IsNullOrEmpty(conditionText))
        {
            return false;
        }

        var match = RawBoConditionRegex.Match(conditionText);
        if (!match.Success)
        {
            return false;
        }

        field = int.Parse(match.Groups["field"].Value) & 7;
        var bitIndex = int.Parse(match.Groups["bit"].Value);
        if (bitIndex is < 0 or > 3)
        {
            return false;
        }

        bit = (CrBit)bitIndex;
        testSet = match.Groups["polarity"].Value == "true";
        return true;
    }

    private static readonly System.Text.RegularExpressions.Regex RawBoConditionRegex = new(
        @"^\(\(true\) && \(\(GetCRBit\(ctx, (?<field>\d+), (?<bit>\d+)\) == (?<polarity>true|false)\)\)\)$",
        System.Text.RegularExpressions.RegexOptions.Compiled |
        System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    /// <summary>
    /// Formats a fused comparison. Integer forms keep the natural relational
    /// operator; float forms keep the PowerPC bit semantics instead, where a
    /// cleared LT bit means "not less than" and is therefore true for NaN.
    /// </summary>
    private static string FusedConditionExpression(
        FusedCompare compare,
        RepresentationEnvironment types,
        Dictionary<string, bool> localPaired)
    {
        if (compare.IsFloat)
        {
            var leftFloat = ToScalarFloatExpression(compare.Left, types, localPaired);
            var rightFloat = ToScalarFloatExpression(compare.Right, types, localPaired);
            return compare.Comparison switch
            {
                FusedComparison.Equal => $"({leftFloat} == {rightFloat})",
                FusedComparison.NotEqual => $"(!({leftFloat} == {rightFloat}))",
                FusedComparison.Less => $"({leftFloat} < {rightFloat})",
                FusedComparison.GreaterOrEqual => $"(!({leftFloat} < {rightFloat}))",
                FusedComparison.Greater => $"({leftFloat} > {rightFloat})",
                _ => $"(!({leftFloat} > {rightFloat}))"
            };
        }

        var cast = compare.IsUnsigned ? "uint32_t" : "int32_t";
        var left = $"static_cast<{cast}>({ToExpression(compare.Left, types)})";
        var right = $"static_cast<{cast}>({ToExpression(compare.Right, types)})";
        var op = compare.Comparison switch
        {
            FusedComparison.Equal => "==",
            FusedComparison.NotEqual => "!=",
            FusedComparison.Less => "<",
            FusedComparison.GreaterOrEqual => ">=",
            FusedComparison.Greater => ">",
            _ => "<="
        };

        return $"({left} {op} {right})";
    }
}
