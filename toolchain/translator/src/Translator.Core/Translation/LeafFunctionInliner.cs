using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

/// <summary>
/// Why a direct call target was not admitted for inlining. Recorded so the
/// classifier can be exercised directly by tests and reported by tooling
/// without re-deriving the decision from emitted text.
/// </summary>
public enum LeafInlineRejection
{
    None = 0,
    ExcludedAddress,
    BlockedAddress,
    DecodeFailed,
    MultipleBlocks,
    NoTerminatingReturn,
    ControlFlow,
    GuestCall,
    OpaqueHelper,
    TouchesLinkRegister,
    TouchesStackPointer,
    Undefined,
    TooLarge,
    FullSynchronizationFence,

    /// <summary>
    /// The callee's control flow contains a back edge. A loop cannot be spliced
    /// by the block-copying path, which reproduces the callee's CFG as a
    /// one-way region between the call point and its continuation.
    /// </summary>
    CyclicControlFlow
}

/// <summary>A leaf callee that may be spliced into its callers.</summary>
/// <param name="EntryPoint">Guest entry point of the callee.</param>
/// <param name="Body">Straight-line IR with the terminating <see cref="IrReturn"/> removed so it falls
/// into the caller's continuation. Empty when <paramref name="Blocks"/> carries the callee instead.</param>
/// <param name="GuestInstructionCount">Distinct guest instructions covered.</param>
/// <param name="Blocks">Null for a straight line, otherwise the callee's own basic blocks; the splice
/// renames labels per call site and turns each return into a jump to the continuation.</param>
public sealed record LeafInlineCandidate(
    uint EntryPoint,
    IReadOnlyList<IrInstruction> Body,
    int GuestInstructionCount,
    IReadOnlyList<IrBasicBlock>? Blocks = null)
{
    /// <summary>
    /// IR instructions the splice adds to the caller, which is what the growth
    /// budget is spent on.
    /// </summary>
    public int SplicedInstructionCount { get; } = Blocks is null
        ? Body.Count
        : Blocks.Sum(static block => block.Instructions.Count);
}

/// <summary>Admission rules for leaf inlining.</summary>
/// <param name="MaxCalleeGuestInstructions">Largest callee that may be spliced.</param>
/// <param name="MaxCallerGrowthPercent">Growth budget as a percentage of the caller's own IR size.</param>
/// <param name="MinCallerGrowthInstructions">Floor for the growth budget, so a small caller can still absorb one leaf.</param>
/// <param name="BlockedTargets">Addresses whose translated body loses to a native override, base-translation
/// exclusion, or mod patch; inlining one would bypass that replacement.</param>
public sealed record LeafInliningPolicy(
    int MaxCalleeGuestInstructions = 32,
    int MaxCallerGrowthPercent = 50,
    int MinCallerGrowthInstructions = 48,
    IReadOnlySet<uint>? BlockedTargets = null,
    /// <summary>
    /// Upper bound on the callee's IR block count. More than one block is admitted only for a
    /// proven straight line, e.g. a <c>cmpwi</c>/<c>blr</c> leaf the decoder splits into a
    /// conditional-branch group of two blocks that still execute in sequence.
    /// </summary>
    int MaxCalleeBlocks = 4,
    /// <summary>
    /// Admit a callee whose blocks are not a straight line, provided its CFG is
    /// a one-way region: no back edge, every block reachable from the entry and
    /// every exit a return. Emergency opt-out for the whole transform.
    /// </summary>
    bool AllowAcyclicMultiBlockCallees = true,
    /// <summary>
    /// Upper bound on the block count of an acyclic multi-block callee. Every
    /// block becomes a block of the caller, so this bounds how much CFG a single
    /// call site can add.
    /// </summary>
    int MaxCalleeAcyclicBlocks = 8)
{
    public bool IsBlocked(uint address) => BlockedTargets?.Contains(address) == true;
}

public sealed record LeafInliningResult(
    IrFunction Function,
    int InlinedCallSites,
    int InlinedGuestInstructions);

/// <summary>
/// IR-level inlining of tiny, call-free leaf functions. Splices on linear IR before SSA construction
/// so the callee's guest instructions become ordinary caller instructions for every downstream analysis.
/// The callee is still emitted standalone too; only the caller's direct-call edge disappears.
/// </summary>
public static class LeafFunctionInliner
{
    /// <summary>
    /// Rewrites <paramref name="function"/> so that every admitted direct call
    /// is replaced by the callee's body. <paramref name="candidateProvider"/>
    /// returns null for a target that cannot be inlined.
    /// </summary>
    public static LeafInliningResult Inline(
        IrFunction function,
        LeafInliningPolicy policy,
        Func<uint, LeafInlineCandidate?> candidateProvider)
    {
        ArgumentNullException.ThrowIfNull(function);
        ArgumentNullException.ThrowIfNull(policy);
        ArgumentNullException.ThrowIfNull(candidateProvider);

        var baseline = function.Blocks.Sum(static block => block.Instructions.Count);
        var allowance = Math.Max(
            policy.MinCallerGrowthInstructions,
            (int)((long)baseline * policy.MaxCallerGrowthPercent / 100));
        var spent = 0;
        var sites = 0;
        var guestInstructions = 0;
        var rewritten = new List<IrBasicBlock>(function.Blocks.Count);
        var changed = false;

        foreach (var block in function.Blocks)
        {
            List<IrInstruction>? output = null;
            // The block being filled. A multi-block splice cuts the caller's
            // block in two, and everything after the call site continues in the
            // continuation block instead of the original one.
            var currentLabel = block.Label;
            var source = block.Instructions;
            for (var index = 0; index < source.Count; index++)
            {
                var instruction = source[index];
                if (spent < allowance &&
                    TryMatchCallSite(source, index, out var target, out var shape) &&
                    candidateProvider(target) is { } candidate &&
                    spent + candidate.SplicedInstructionCount <= allowance)
                {
                    output ??= new List<IrInstruction>(source.Take(index));
                    if (shape == CallSiteShape.Linked)
                    {
                        // Drop the `lr = <return address>` the call form wrote; nothing can observe it
                        // once the call is gone, since the callee is proven not to touch LR.
                        output.RemoveAt(output.Count - 1);
                    }

                    output.Add(new IrComment(
                        $"inline leaf 0x{candidate.EntryPoint:X8} ({candidate.GuestInstructionCount} guest instruction(s))"));
                    if (candidate.Blocks is null)
                    {
                        output.AddRange(candidate.Body);
                        output.Add(new IrComment($"end of inlined leaf 0x{candidate.EntryPoint:X8}"));
                    }
                    else
                    {
                        // The callee's blocks become caller blocks between the call site and the
                        // continuation. Labels are suffixed with the call site's ordinal, so the same
                        // callee can be spliced twice and no spliced label reads back as a guest address.
                        var continuation = ContinuationLabel(sites, candidate.EntryPoint);
                        var spliced = RenameSplicedBlocks(candidate.Blocks, sites, continuation);
                        output.Add(new IrJump(spliced[0].Label));
                        rewritten.Add(new IrBasicBlock(currentLabel, output));
                        rewritten.AddRange(spliced);
                        currentLabel = continuation;
                        output = new List<IrInstruction>
                        {
                            new IrComment($"end of inlined leaf 0x{candidate.EntryPoint:X8}")
                        };
                    }

                    spent += candidate.SplicedInstructionCount;
                    sites++;
                    guestInstructions += candidate.GuestInstructionCount;
                    changed = true;
                    continue;
                }

                output?.Add(instruction);
            }

            rewritten.Add(output is null ? block : new IrBasicBlock(currentLabel, output));
        }

        return changed
            ? new LeafInliningResult(
                new IrFunction(function.Name, function.EntryLabel, rewritten), sites, guestInstructions)
            : new LeafInliningResult(function, 0, 0);
    }

    /// <summary>
    /// Classifies a decoded callee, returning null and a reason when it's outside the safe subset.
    /// Tried against the straight-line classifier first, so a body admitted before this file grew a
    /// second shape still produces the same splice.
    /// </summary>
    public static LeafInlineCandidate? TryCreateCandidate(
        uint entryPoint,
        IrFunction callee,
        IReadOnlyList<PpcInstruction> calleeInstructions,
        LeafInliningPolicy policy,
        out LeafInlineRejection rejection)
    {
        ArgumentNullException.ThrowIfNull(callee);
        ArgumentNullException.ThrowIfNull(policy);

        // A replacement anywhere inside the decoded body counts, not just at the
        // entry: splicing would silently keep running the original bytes of a
        // region that the runtime resolves elsewhere.
        if (policy.IsBlocked(entryPoint) ||
            (calleeInstructions is not null &&
             calleeInstructions.Any(instruction => policy.IsBlocked(instruction.Address))))
        {
            rejection = LeafInlineRejection.BlockedAddress;
            return null;
        }

        if (callee.Blocks.Count == 0 ||
            !string.Equals(callee.Blocks[0].Label, callee.EntryLabel, StringComparison.OrdinalIgnoreCase))
        {
            rejection = LeafInlineRejection.MultipleBlocks;
            return null;
        }

        var straightLine = TryCreateStraightLineCandidate(entryPoint, callee, policy, out rejection);
        if (straightLine is not null)
        {
            return straightLine;
        }

        if (!policy.AllowAcyclicMultiBlockCallees || callee.Blocks.Count < 2)
        {
            return null;
        }

        var acyclic = TryCreateAcyclicCandidate(entryPoint, callee, policy, out var acyclicRejection);
        if (acyclic is not null)
        {
            rejection = LeafInlineRejection.None;
            return acyclic;
        }

        // Both shapes refused. "This is not a straight line" is no longer a
        // reason on its own, so whichever scan found a substantive blocker gets
        // to report it; when the acyclic scan only objected to the shape as
        // well, the straight-line verdict stands.
        if (acyclicRejection is not (LeafInlineRejection.ControlFlow or LeafInlineRejection.MultipleBlocks))
        {
            rejection = acyclicRejection;
        }

        return null;
    }

    /// <summary>
    /// The original admission rule: one to <see cref="LeafInliningPolicy.MaxCalleeBlocks"/>
    /// blocks that provably execute as one straight line, spliced as a flat
    /// statement list.
    /// </summary>
    private static LeafInlineCandidate? TryCreateStraightLineCandidate(
        uint entryPoint,
        IrFunction callee,
        LeafInliningPolicy policy,
        out LeafInlineRejection rejection)
    {
        if (callee.Blocks.Count > policy.MaxCalleeBlocks)
        {
            rejection = LeafInlineRejection.MultipleBlocks;
            return null;
        }

        // Concatenating the blocks is only sound for a straight line. The
        // per-instruction check below rejects every control transfer, and the
        // guest addresses are then verified to run contiguously from the entry
        // point, which proves the blocks execute in this order with nothing
        // else able to enter them.
        var instructions = callee.Blocks.Count == 1
            ? callee.Blocks[0].Instructions
            : callee.Blocks.SelectMany(static block => block.Instructions).ToArray();
        if (instructions.Count == 0 || instructions[^1] is not IrReturn)
        {
            rejection = LeafInlineRejection.NoTerminatingReturn;
            return null;
        }

        var guestAddresses = new HashSet<uint>();
        var expectedAddress = entryPoint;
        for (var index = 0; index < instructions.Count - 1; index++)
        {
            var instruction = instructions[index];
            if (instruction is IrTracePpc trace)
            {
                if (trace.Address != expectedAddress)
                {
                    rejection = LeafInlineRejection.ControlFlow;
                    return null;
                }

                expectedAddress += 4;
                guestAddresses.Add(trace.Address);
                continue;
            }

            rejection = ClassifyBodyInstruction(instruction);
            if (rejection != LeafInlineRejection.None)
            {
                return null;
            }
        }

        if (guestAddresses.Count > policy.MaxCalleeGuestInstructions)
        {
            rejection = LeafInlineRejection.TooLarge;
            return null;
        }

        var wholeBodyRejection = ClassifyWholeBody(callee);
        if (wholeBodyRejection != LeafInlineRejection.None)
        {
            rejection = wholeBodyRejection;
            return null;
        }

        rejection = LeafInlineRejection.None;
        // The terminating blr becomes a fall-through into the caller.
        return new LeafInlineCandidate(
            entryPoint,
            instructions.Take(instructions.Count - 1).ToArray(),
            guestAddresses.Count);
    }

    /// <summary>
    /// Admits a callee whose blocks form a one-way region (reachable from entry, no back edge, every
    /// exit a plain return) instead of a straight line. Label renaming plus rewriting returns as jumps
    /// to one continuation reconnects it; the no-back-edge rule is what keeps it a region, not a loop.
    /// </summary>
    private static LeafInlineCandidate? TryCreateAcyclicCandidate(
        uint entryPoint,
        IrFunction callee,
        LeafInliningPolicy policy,
        out LeafInlineRejection rejection)
    {
        if (callee.Blocks.Count > policy.MaxCalleeAcyclicBlocks)
        {
            rejection = LeafInlineRejection.MultipleBlocks;
            return null;
        }

        var indexByLabel = new Dictionary<string, int>(
            callee.Blocks.Count, StringComparer.OrdinalIgnoreCase);
        for (var index = 0; index < callee.Blocks.Count; index++)
        {
            if (!indexByLabel.TryAdd(callee.Blocks[index].Label, index))
            {
                rejection = LeafInlineRejection.ControlFlow;
                return null;
            }
        }

        var normalized = new IrBasicBlock[callee.Blocks.Count];
        var successors = new int[callee.Blocks.Count][];
        var guestAddresses = new HashSet<uint>();
        var hasReturn = false;

        for (var index = 0; index < callee.Blocks.Count; index++)
        {
            var block = callee.Blocks[index];
            if (block.Instructions.Count == 0)
            {
                // The CFG builder gives an empty block no successor at all while
                // the emitter falls through it. Refuse the disagreement rather
                // than pick a side.
                rejection = LeafInlineRejection.ControlFlow;
                return null;
            }

            var terminator = block.Instructions[^1];
            switch (terminator)
            {
                case IrReturn { Value: null }:
                    successors[index] = Array.Empty<int>();
                    normalized[index] = block;
                    hasReturn = true;
                    break;
                case IrJump jump when indexByLabel.TryGetValue(jump.TargetLabel, out var jumpTarget):
                    successors[index] = [jumpTarget];
                    normalized[index] = block;
                    break;
                case IrBranch branch
                    when indexByLabel.TryGetValue(branch.TrueLabel, out var trueTarget) &&
                         indexByLabel.TryGetValue(branch.FalseLabel, out var falseTarget):
                    successors[index] = [trueTarget, falseTarget];
                    normalized[index] = block;
                    break;
                case IrReturn or IrJump or IrBranch or IrIndirectJump or IrJumpTable:
                    // A return that carries a value, or a transfer to a label
                    // outside the decoded body.
                    rejection = LeafInlineRejection.ControlFlow;
                    return null;
                case IrUndefined:
                    rejection = LeafInlineRejection.Undefined;
                    return null;
                default:
                    if (index + 1 >= callee.Blocks.Count)
                    {
                        rejection = LeafInlineRejection.NoTerminatingReturn;
                        return null;
                    }

                    successors[index] = [index + 1];
                    normalized[index] = new IrBasicBlock(
                        block.Label,
                        [.. block.Instructions, new IrJump(callee.Blocks[index + 1].Label)]);
                    break;
            }

            var scanCount = terminator is IrReturn or IrJump or IrBranch
                ? block.Instructions.Count - 1
                : block.Instructions.Count;
            var sawGuestInstruction = false;
            for (var position = 0; position < scanCount; position++)
            {
                var instruction = block.Instructions[position];
                if (instruction is IrTracePpc trace)
                {
                    if (!guestAddresses.Add(trace.Address))
                    {
                        // The same guest instruction reached twice means the
                        // decode overlapped itself, not a region.
                        rejection = LeafInlineRejection.ControlFlow;
                        return null;
                    }

                    sawGuestInstruction = true;
                    continue;
                }

                rejection = ClassifyBodyInstruction(instruction);
                if (rejection != LeafInlineRejection.None)
                {
                    return null;
                }
            }

            if (!sawGuestInstruction &&
                (block.Instructions.Count != 1 || block.Instructions[0] is not IrReturn))
            {
                // The synthetic return block is the only block the front end
                // creates with no guest instruction behind it. Anything else
                // with no trace is a stand-in for a target that was never
                // decoded, and its behaviour is not this callee's.
                rejection = LeafInlineRejection.ControlFlow;
                return null;
            }
        }

        if (!hasReturn)
        {
            rejection = LeafInlineRejection.NoTerminatingReturn;
            return null;
        }

        if (guestAddresses.Count == 0)
        {
            rejection = LeafInlineRejection.ControlFlow;
            return null;
        }

        if (guestAddresses.Count > policy.MaxCalleeGuestInstructions)
        {
            rejection = LeafInlineRejection.TooLarge;
            return null;
        }

        // The decoded body must be exactly the contiguous run that starts at the
        // entry point. A gap, or an address below the entry, means the decode
        // covered something other than the function this splice claims to be.
        var lastAddress = entryPoint + (uint)((guestAddresses.Count - 1) * 4);
        foreach (var address in guestAddresses)
        {
            if (address < entryPoint || address > lastAddress || (address - entryPoint) % 4 != 0)
            {
                rejection = LeafInlineRejection.ControlFlow;
                return null;
            }
        }

        var visitState = new byte[normalized.Length];
        if (!IsAcyclicFrom(0, successors, visitState))
        {
            rejection = LeafInlineRejection.CyclicControlFlow;
            return null;
        }

        foreach (var state in visitState)
        {
            if (state != VisitedCompletely)
            {
                // An unreachable block is dead code the caller would still have
                // to carry, and it is evidence the decode is not the shape this
                // classifier thinks it is.
                rejection = LeafInlineRejection.ControlFlow;
                return null;
            }
        }

        var wholeBodyRejection = ClassifyWholeBody(callee);
        if (wholeBodyRejection != LeafInlineRejection.None)
        {
            rejection = wholeBodyRejection;
            return null;
        }

        rejection = LeafInlineRejection.None;
        return new LeafInlineCandidate(
            entryPoint, Array.Empty<IrInstruction>(), guestAddresses.Count, normalized);
    }

    private const byte VisitedInProgress = 1;
    private const byte VisitedCompletely = 2;

    /// <summary>
    /// Depth-first walk that fails on the first back edge and marks everything
    /// it reaches, so the caller can also read off reachability.
    /// </summary>
    private static bool IsAcyclicFrom(int index, int[][] successors, byte[] visitState)
    {
        visitState[index] = VisitedInProgress;
        foreach (var next in successors[index])
        {
            if (visitState[next] == VisitedInProgress)
            {
                return false;
            }

            if (visitState[next] != VisitedCompletely && !IsAcyclicFrom(next, successors, visitState))
            {
                return false;
            }
        }

        visitState[index] = VisitedCompletely;
        return true;
    }

    /// <summary>
    /// Per-instruction rules shared by both admission shapes: no control transfer, nothing opaque,
    /// nothing touching the link register whose write the call form just dropped. Terminators are
    /// classified by the callers, which know where a block ends.
    /// </summary>
    private static LeafInlineRejection ClassifyBodyInstruction(IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrBranch or IrJump or IrIndirectJump or IrJumpTable or IrReturn or IrPhi:
                return LeafInlineRejection.ControlFlow;
            case IrIndirectCall:
                return LeafInlineRejection.GuestCall;
            case IrUndefined:
                return LeafInlineRejection.Undefined;
            case IrCall call:
                if (TryParseGuestAddress(call.Target, out _))
                {
                    return LeafInlineRejection.GuestCall;
                }

                var effect = GuestHelperEffectCatalog.Analyze(call);
                if (effect.BoundaryFlags != GuestCallBoundaryFlags.None)
                {
                    // Unknown helpers and every synchronization boundary
                    // (OSSystemCall and friends) are complete-context by
                    // construction, so this also covers requirement of no
                    // full-synchronization fences inside the callee.
                    return LeafInlineRejection.OpaqueHelper;
                }

                if (effect.ReadsLr || effect.WritesLr)
                {
                    return LeafInlineRejection.TouchesLinkRegister;
                }

                break;
        }

        return TouchesLinkRegister(instruction)
            ? LeafInlineRejection.TouchesLinkRegister
            : LeafInlineRejection.None;
    }

    /// <summary>
    /// Rejects effects only visible once the whole body is analyzed. A callee that adjusts r1 would
    /// create a nested stack-pointer epoch when copied into the caller, but the stack-address/save-restore
    /// passes model one entry stack pointer per function, risking aliased caller stack slots.
    /// </summary>
    private static LeafInlineRejection ClassifyWholeBody(IrFunction callee)
    {
        var contract = GuestAbiContractAnalyzer.Analyze(callee);
        if (contract.HasFullSynchronizationFence)
        {
            return LeafInlineRejection.FullSynchronizationFence;
        }

        return (contract.GprPossibleWriteMask & (1u << 1)) != 0
            ? LeafInlineRejection.TouchesStackPointer
            : LeafInlineRejection.None;
    }

    /// <summary>
    /// Label of the block the spliced region's returns jump to, which is where
    /// the caller resumes. It cannot be read back as a guest address, so the
    /// emitter never mistakes it for an interior resume point of the caller.
    /// </summary>
    private static string ContinuationLabel(int callSiteOrdinal, uint entryPoint) =>
        $"inl{callSiteOrdinal}_cont_{entryPoint:X8}";

    private static string SplicedBlockLabel(int callSiteOrdinal, string calleeLabel) =>
        $"inl{callSiteOrdinal}_{calleeLabel}";

    /// <summary>
    /// Copies the callee's blocks under call-site-local labels and reconnects their returns to
    /// <paramref name="continuation"/>. The classifier already proved return/jump/branch only
    /// appear as block terminators, so the rewrite below cannot fire on anything else.
    /// </summary>
    private static IrBasicBlock[] RenameSplicedBlocks(
        IReadOnlyList<IrBasicBlock> blocks, int callSiteOrdinal, string continuation)
    {
        // Targets are resolved through the block they name rather than by
        // suffixing the target string, so a transfer and its block agree
        // character for character even where the front end spelled the same
        // label with different casing.
        var renamedByLabel = new Dictionary<string, string>(
            blocks.Count, StringComparer.OrdinalIgnoreCase);
        foreach (var block in blocks)
        {
            renamedByLabel[block.Label] = SplicedBlockLabel(callSiteOrdinal, block.Label);
        }

        string Target(string label) => renamedByLabel.TryGetValue(label, out var renamed)
            ? renamed
            : SplicedBlockLabel(callSiteOrdinal, label);

        var spliced = new IrBasicBlock[blocks.Count];
        for (var index = 0; index < blocks.Count; index++)
        {
            var block = blocks[index];
            var instructions = new List<IrInstruction>(block.Instructions.Count);
            foreach (var instruction in block.Instructions)
            {
                instructions.Add(instruction switch
                {
                    IrReturn => new IrJump(continuation),
                    IrJump jump => new IrJump(Target(jump.TargetLabel)),
                    IrBranch branch => branch with
                    {
                        TrueLabel = Target(branch.TrueLabel),
                        FalseLabel = Target(branch.FalseLabel)
                    },
                    _ => instruction
                });
            }

            spliced[index] = new IrBasicBlock(renamedByLabel[block.Label], instructions);
        }

        return spliced;
    }

    internal enum CallSiteShape
    {
        Linked,
        Tail
    }

    /// <summary>
    /// Recognizes only the two direct-call forms the lifter produces: <c>bl</c> lowers to
    /// <c>lr = &lt;return address&gt;</c> then the call, and <c>b</c> to an external function lowers to
    /// a call immediately followed by the caller's return.
    /// </summary>
    internal static bool TryMatchCallSite(
        IReadOnlyList<IrInstruction> instructions,
        int index,
        out uint target,
        out CallSiteShape shape)
    {
        target = 0;
        shape = CallSiteShape.Linked;
        if (instructions[index] is not IrCall call ||
            !string.IsNullOrWhiteSpace(call.Destination) ||
            !TryParseGuestAddress(call.Target, out target))
        {
            return false;
        }

        // Every decoded guest instruction contributes one trace, appended after
        // the call it was lifted from.
        if (index + 1 >= instructions.Count || instructions[index + 1] is not IrTracePpc trace)
        {
            return false;
        }

        if (index > 0 &&
            instructions[index - 1] is IrAssign { Value.Kind: "const" } assign &&
            IsLinkRegister(assign.Destination))
        {
            if (assign.Value.Constant is not { } linkValue ||
                unchecked((uint)linkValue) != trace.Address + 4u)
            {
                return false;
            }

            shape = CallSiteShape.Linked;
            return true;
        }

        if (index + 2 < instructions.Count && instructions[index + 2] is IrReturn)
        {
            shape = CallSiteShape.Tail;
            return true;
        }

        return false;
    }

    internal static bool TryParseGuestAddress(string? target, out uint address)
    {
        address = 0;
        return target is { Length: > 2 } &&
               target.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
               uint.TryParse(
                   target.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out address);
    }

    private static bool TouchesLinkRegister(IrInstruction instruction)
    {
        foreach (var name in IrRegisterDataFlow.Definitions(instruction))
        {
            if (IsLinkRegister(name)) return true;
        }

        foreach (var name in IrRegisterDataFlow.Uses(instruction))
        {
            if (IsLinkRegister(name)) return true;
        }

        return false;
    }

    private static bool IsLinkRegister(string name) =>
        RegisterNameUtils.HardwareBase(name).Equals("lr", StringComparison.OrdinalIgnoreCase);
}
