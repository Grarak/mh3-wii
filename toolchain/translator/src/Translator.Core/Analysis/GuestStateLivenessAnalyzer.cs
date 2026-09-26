using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Architectural state that can remain as ordinary SSA values between known translated call
/// sites. Memory is deliberately absent, it is an independently observable side effect, not part of the register file.
/// </summary>
public readonly record struct GuestStateMask(
    uint Gpr,
    uint Fpr,
    byte Cr,
    bool Xer,
    bool Ctr,
    bool Lr,
    bool Fpscr,
    byte Gqr,
    byte Hid)
{
    public static GuestStateMask Empty => default;

    public bool IsEmpty =>
        Gpr == 0 && Fpr == 0 && Cr == 0 && !Xer && !Ctr && !Lr && !Fpscr && Gqr == 0 && Hid == 0;

    public GuestStateMask Union(GuestStateMask other) =>
        new(Gpr | other.Gpr, Fpr | other.Fpr, (byte)(Cr | other.Cr),
            Xer || other.Xer, Ctr || other.Ctr, Lr || other.Lr,
            Fpscr || other.Fpscr, (byte)(Gqr | other.Gqr), (byte)(Hid | other.Hid));

    public GuestStateMask Intersect(GuestStateMask other) =>
        new(Gpr & other.Gpr, Fpr & other.Fpr, (byte)(Cr & other.Cr),
            Xer && other.Xer, Ctr && other.Ctr, Lr && other.Lr,
            Fpscr && other.Fpscr, (byte)(Gqr & other.Gqr), (byte)(Hid & other.Hid));

    public GuestStateMask Except(GuestStateMask other) =>
        new(Gpr & ~other.Gpr, Fpr & ~other.Fpr, (byte)(Cr & ~other.Cr),
            Xer && !other.Xer, Ctr && !other.Ctr, Lr && !other.Lr,
            Fpscr && !other.Fpscr, (byte)(Gqr & ~other.Gqr), (byte)(Hid & ~other.Hid));

    public static GuestStateMask FromContractReads(GuestAbiContract contract) =>
        new(contract.GprReadBeforeWriteMask, contract.FprReadBeforeWriteMask,
            contract.CrReadBeforeWriteMask, contract.ReadsXerBeforeWrite,
            contract.ReadsCtrBeforeWrite, contract.ReadsLrBeforeWrite,
            contract.ReadsFpscrBeforeWrite, contract.GqrReadBeforeWriteMask,
            contract.HidReadBeforeWriteMask);

    public static GuestStateMask FromContractWrites(GuestAbiContract contract) =>
        new(contract.GprPossibleWriteMask, contract.FprPossibleWriteMask,
            contract.CrPossibleWriteMask, contract.MayWriteXer,
            contract.MayWriteCtr, contract.MayWriteLr,
            contract.MayWriteFpscr, contract.GqrPossibleWriteMask,
            contract.HidPossibleWriteMask);

    public static GuestStateMask FromContractDefiniteWrites(GuestAbiContract contract) =>
        new(contract.GprDefiniteWriteMask, contract.FprDefiniteWriteMask,
            contract.CrDefiniteWriteMask, contract.DefinitelyWritesXer,
            contract.DefinitelyWritesCtr, contract.DefinitelyWritesLr,
            contract.DefinitelyWritesFpscr, contract.GqrDefiniteWriteMask,
            contract.HidDefiniteWriteMask);
}

public sealed record GuestCallSiteStateContract(
    string BlockLabel,
    int InstructionIndex,
    int CallOrdinal,
    uint Target,
    GuestStateMask Inputs,
    GuestStateMask Outputs,
    GuestStateMask LiveAfter);

/// <summary>
/// Stable identity for a direct call across codegen rewrites that remove unrelated instructions.
/// The ordinal counts same-target calls in a block, so it survives rewrites that InstructionIndex does not.
/// </summary>
public readonly record struct GuestStateFreeCallSiteKey(
    string BlockLabel,
    uint Target,
    int CallOrdinal);

/// <summary>
/// A compact native entry specialized for the state consumed after one or more
/// equivalent direct call sites.
/// </summary>
public sealed record GuestStateFreeCallVariant(
    uint Target,
    string Symbol,
    GuestAbiContract Contract);

public sealed record GuestStateLivenessResult(
    IReadOnlyDictionary<string, GuestStateMask> BlockLiveIn,
    IReadOnlyDictionary<string, GuestStateMask> BlockLiveOut,
    IReadOnlyList<GuestCallSiteStateContract> DirectCalls);

/// <summary>
/// Backward architectural-state liveness. A call kills the values it writes, introduces only its
/// proven inputs, and reports only values live after that exact call site.
/// </summary>
public static class GuestStateLivenessAnalyzer
{
    /// <summary>
    /// A public translated-function boundary materializes CpuContext, so every value the function
    /// may change is observable there. Guest ABI return masks describe source-language return
    /// values only, not a license to discard other instruction-level side effects.
    /// </summary>
    public static GuestStateMask MaterializedContextExit(GuestAbiContract contract) =>
        GuestStateMask.FromContractWrites(contract);

    public static uint RequiredStateFreeGprInputs(GuestAbiContract contract, uint demandedOutputMask)
    {
        var outputs = contract.GprPossibleWriteMask & demandedOutputMask;
        return contract.GprReadBeforeWriteMask | (outputs & ~contract.GprDefiniteWriteMask);
    }

    public static bool CanDeconstructWithoutContext(IrFunction function)
    {
        foreach (var instruction in function.Blocks.SelectMany(static block => block.Instructions))
        {
            // Unknown control-flow targets have no static call-site contract,
            // so the dispatcher must observe a materialized CpuContext.
            if (instruction is IrIndirectCall or IrIndirectJump or IrJumpTable or IrUndefined)
                return false;

            foreach (var name in IrRegisterDataFlow.Uses(instruction))
            {
                if (IsSupervisorRegister(name)) return false;
            }

            foreach (var name in IrRegisterDataFlow.Definitions(instruction))
            {
                if (IsSupervisorRegister(name)) return false;
            }
        }
        return true;
    }

    /// <summary>
    /// Supervisor-level state the state-free calling convention cannot carry.
    /// </summary>
    private static bool IsSupervisorRegister(string name)
    {
        var baseName = RegisterNameUtils.HardwareBase(name.AsSpan());
        return baseName.Equals("msr", StringComparison.Ordinal) ||
               baseName.Equals("dar", StringComparison.Ordinal) ||
               baseName.Equals("dsisr", StringComparison.Ordinal) ||
               baseName.Equals("iccr", StringComparison.Ordinal) ||
               baseName.Equals("tbr", StringComparison.Ordinal) ||
               baseName.Equals("tbl", StringComparison.Ordinal) ||
               baseName.Equals("tbu", StringComparison.Ordinal) ||
               baseName.Equals("srr0", StringComparison.Ordinal) ||
               baseName.Equals("srr1", StringComparison.Ordinal);
    }

    public static GuestStateLivenessResult Analyze(
        IrFunction function,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts,
        GuestStateMask exitLive)
    {
        var blockList = function.Blocks;
        var blockCount = blockList.Count;
        var blocks = blockList.ToDictionary(block => block.Label, StringComparer.Ordinal);
        // Successor labels are resolved once into per-block arrays; the fixpoint
        // below re-reads them on every round and cannot afford the projection.
        var successors = new string[blockCount][];
        for (var index = 0; index < blockCount; ++index)
        {
            var successorCount = Successors(
                blockList[index],
                index + 1 < blockCount ? blockList[index + 1].Label : null,
                out var first,
                out var second);
            var keepFirst = successorCount >= 1 && blocks.ContainsKey(first);
            var keepSecond = successorCount == 2 && blocks.ContainsKey(second);
            successors[index] =
                keepFirst && keepSecond ? new[] { first, second }
                : keepFirst ? new[] { first }
                : keepSecond ? new[] { second }
                : Array.Empty<string>();
        }

        var liveIn = blockList.ToDictionary(block => block.Label, _ => GuestStateMask.Empty, StringComparer.Ordinal);
        var liveOut = blockList.ToDictionary(block => block.Label, _ => GuestStateMask.Empty, StringComparer.Ordinal);

        bool changed;
        do
        {
            changed = false;
            for (var blockIndex = blockCount - 1; blockIndex >= 0; --blockIndex)
            {
                var block = blockList[blockIndex];
                var blockSuccessors = successors[blockIndex];
                var outgoing = exitLive;
                if (blockSuccessors.Length != 0)
                {
                    outgoing = GuestStateMask.Empty;
                    foreach (var label in blockSuccessors) outgoing = outgoing.Union(liveIn[label]);
                }

                var incoming = TransferBlock(block, outgoing, calleeContracts, calls: null);
                if (outgoing != liveOut[block.Label] || incoming != liveIn[block.Label])
                {
                    liveOut[block.Label] = outgoing;
                    liveIn[block.Label] = incoming;
                    changed = true;
                }
            }
        } while (changed);

        var calls = new List<GuestCallSiteStateContract>();
        foreach (var block in function.Blocks)
            TransferBlock(block, liveOut[block.Label], calleeContracts, calls);
        calls.Sort(static (left, right) =>
        {
            var block = string.CompareOrdinal(left.BlockLabel, right.BlockLabel);
            return block != 0 ? block : left.InstructionIndex.CompareTo(right.InstructionIndex);
        });
        var ordinals = new Dictionary<(string Block, uint Target), int>();
        for (var index = 0; index < calls.Count; ++index)
        {
            var call = calls[index];
            var key = (call.BlockLabel, call.Target);
            var ordinal = ordinals.GetValueOrDefault(key);
            calls[index] = call with { CallOrdinal = ordinal };
            ordinals[key] = ordinal + 1;
        }
        return new GuestStateLivenessResult(liveIn, liveOut, calls);
    }

    private static GuestStateMask TransferBlock(
        IrBasicBlock block,
        GuestStateMask live,
        IReadOnlyDictionary<uint, GuestAbiContract> calleeContracts,
        List<GuestCallSiteStateContract>? calls)
    {
        for (var index = block.Instructions.Count - 1; index >= 0; --index)
        {
            var instruction = block.Instructions[index];
            if (instruction is IrCall call &&
                GuestTargetParser.TryParseAddress(call.Target, out var target) &&
                calleeContracts.TryGetValue(target, out var contract))
            {
                var inputs = GuestStateMask.FromContractReads(contract);
                // A no-destination call is a lowered PPC tail branch, which forwards the caller's
                // incoming LR to the eventual blr even if the callee's ABI summary omits it.
                if (string.IsNullOrWhiteSpace(call.Destination))
                    inputs = inputs.Union(GuestStateMask.Empty with { Lr = true });
                var possibleWrites = GuestStateMask.FromContractWrites(contract);
                var definiteWrites = GuestStateMask.FromContractDefiniteWrites(contract);
                var liveAfter = live;
                calls?.Add(new GuestCallSiteStateContract(
                    block.Label, index, 0, target, inputs, liveAfter.Intersect(possibleWrites), liveAfter));
                live = liveAfter.Except(definiteWrites).Union(inputs);
                // The IR call destination (normally LR) is an instruction-level
                // definition in addition to the callee's architectural effects.
                var destination = Register(call.Destination);
                live = live.Except(destination);
                continue;
            }

            live = live.Except(Writes(instruction)).Union(Reads(instruction));
        }
        return live;
    }

    private static GuestStateMask Reads(IrInstruction instruction)
    {
        var result = GuestStateMask.Empty;
        switch (instruction)
        {
            case IrAssign value: AddValue(ref result, value.Value); break;
            case IrBinary value: AddValue(ref result, value.Left); AddValue(ref result, value.Right); break;
            case IrLoad value: Add(ref result, value.Address.Base); break;
            case IrStore value: Add(ref result, value.Address.Base); AddValue(ref result, value.Source); break;
            case IrResolveGuestMemoryRange value: AddValue(ref result, value.Base); break;
            case IrResolvedLoad value: Add(ref result, value.OriginalAddress.Base); break;
            case IrResolvedStore value: Add(ref result, value.OriginalAddress.Base); AddValue(ref result, value.Source); break;
            case IrResolvedPsqLoad value:
                AddValue(ref result, value.OriginalAddress);
                if (value.KnownGqr is null || value.GuardKnownGqr) AddGqr(ref result, value.I);
                break;
            case IrResolvedPsqStore value:
                AddValue(ref result, value.OriginalAddress); AddValue(ref result, value.Source);
                if (value.KnownGqr is null || value.GuardKnownGqr) AddGqr(ref result, value.I);
                break;
            case IrResolvedLoadPair value:
                Add(ref result, value.FirstOriginalAddress.Base);
                Add(ref result, value.SecondOriginalAddress.Base);
                break;
            case IrResolvedStorePair value:
                Add(ref result, value.FirstOriginalAddress.Base); Add(ref result, value.SecondOriginalAddress.Base);
                AddValue(ref result, value.FirstSource); AddValue(ref result, value.SecondSource);
                break;
            case IrCall value: foreach (var argument in value.Arguments) AddValue(ref result, argument); break;
            case IrIndirectCall value:
                AddValue(ref result, value.Target);
                foreach (var argument in value.Arguments) AddValue(ref result, argument);
                result = result.Union(FullState);
                break;
            case IrIndirectJump value: AddValue(ref result, value.Target); result = result.Union(FullState); break;
            case IrSetCrField value:
                AddValue(ref result, value.Left);
                AddValue(ref result, value.Right);
                // Same as unioning Register("xer").
                result = result with { Xer = true };
                break;
            case IrPhi value: foreach (var source in value.Sources.Values) Add(ref result, source); break;
            case IrBranch value: Add(ref result, value.ConditionRegister); break;
            case IrJumpTable value: Add(ref result, value.Selector); break;
            case IrReturn { Value: { } value }: AddValue(ref result, value); break;
        }
        return result;
    }

    private static GuestStateMask Writes(IrInstruction instruction)
    {
        var result = GuestStateMask.Empty;
        switch (instruction)
        {
            case IrAssign value: Add(ref result, value.Destination); break;
            case IrBinary value: Add(ref result, value.Destination); break;
            case IrLoad value: Add(ref result, value.Destination); break;
            case IrResolveGuestMemoryRange value: Add(ref result, value.Destination); break;
            case IrResolvedLoad value: Add(ref result, value.Destination); break;
            case IrResolvedPsqLoad value: Add(ref result, value.Destination); break;
            case IrResolvedLoadPair value:
                Add(ref result, value.FirstDestination);
                Add(ref result, value.SecondDestination);
                break;
            case IrCall value: Add(ref result, value.Destination); break;
            case IrIndirectCall value: Add(ref result, value.Destination); result = result.Union(FullState); break;
            case IrSetCrField value:
                result = result with { Cr = (byte)(result.Cr | (1 << value.FieldIndex)) };
                break;
            case IrPhi value: Add(ref result, value.Destination); break;
            case IrUndefined: result = FullState; break;
        }
        return result;
    }

    private static void Add(ref GuestStateMask mask, string? name) => mask = mask.Union(Register(name));

    private static void AddValue(ref GuestStateMask mask, IrValue value)
    {
        if (value.Kind == "register") Add(ref mask, value.RegisterName);
    }

    private static void AddGqr(ref GuestStateMask mask, uint index)
    {
        // Mirrors Register("gqr{index}"): only the eight architectural
        // quantization registers exist, anything else names nothing.
        if (index <= 7) mask = mask with { Gqr = (byte)(mask.Gqr | (1 << (int)index)) };
    }

    private static GuestStateMask Register(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return GuestStateMask.Empty;
        var baseName = RegisterNameUtils.HardwareBase(name.AsSpan());
        if (baseName.Length >= 2 && baseName[0] == 'r' && int.TryParse(baseName[1..], out var gpr) && gpr is >= 0 and < 32)
            return GuestStateMask.Empty with { Gpr = 1u << gpr };
        if (baseName.Length >= 2 && baseName[0] == 'f' && int.TryParse(baseName[1..], out var fpr) && fpr is >= 0 and < 32)
            return GuestStateMask.Empty with { Fpr = 1u << fpr };
        if (baseName.Length == 3 && baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) && baseName[2] is >= '0' and <= '7')
            return GuestStateMask.Empty with { Cr = (byte)(1 << (baseName[2] - '0')) };
        if (baseName.Length is >= 4 and <= 5 && baseName.StartsWith("crb", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(baseName[3..], out var crBit) && crBit is >= 0 and < 32)
            return GuestStateMask.Empty with { Cr = (byte)(1 << (crBit / 4)) };
        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase)) return GuestStateMask.Empty with { Cr = byte.MaxValue };
        if (baseName.Equals("xer", StringComparison.OrdinalIgnoreCase)) return GuestStateMask.Empty with { Xer = true };
        if (baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase)) return GuestStateMask.Empty with { Ctr = true };
        if (baseName.Equals("lr", StringComparison.OrdinalIgnoreCase)) return GuestStateMask.Empty with { Lr = true };
        if (baseName.Equals("fpscr", StringComparison.OrdinalIgnoreCase)) return GuestStateMask.Empty with { Fpscr = true };
        if (baseName.Length == 4 && baseName.StartsWith("gqr", StringComparison.OrdinalIgnoreCase) &&
            baseName[3] is >= '0' and <= '7')
            return GuestStateMask.Empty with { Gqr = (byte)(1 << (baseName[3] - '0')) };
        if (baseName.Length == 4 && baseName.StartsWith("hid", StringComparison.OrdinalIgnoreCase) &&
            baseName[3] is >= '0' and <= '2')
            return GuestStateMask.Empty with { Hid = (byte)(1 << (baseName[3] - '0')) };
        return GuestStateMask.Empty;
    }

    /// <summary>
    /// Returns the number of syntactic successors of <paramref name="block"/>
    /// (0, 1 or 2) and their labels, without allocating an iterator.
    /// </summary>
    private static int Successors(
        IrBasicBlock block,
        string? fallthroughLabel,
        out string first,
        out string second)
    {
        var instructions = block.Instructions;
        var terminator = instructions.Count == 0 ? null : instructions[instructions.Count - 1];
        if (terminator is IrBranch branch)
        {
            first = branch.TrueLabel;
            second = branch.FalseLabel;
            return 2;
        }

        if (terminator is IrJump jump)
        {
            first = jump.TargetLabel;
            second = string.Empty;
            return 1;
        }

        if (terminator is not (IrReturn or IrUndefined or IrIndirectJump or IrJumpTable) &&
            fallthroughLabel is not null)
        {
            // The canonical IR deliberately leaves ordinary sequential PPC
            // flow implicit.  Omitting this edge makes later-block inputs look
            // dead at entry and is unsound for state deconstruction.
            first = fallthroughLabel;
            second = string.Empty;
            return 1;
        }

        first = string.Empty;
        second = string.Empty;
        return 0;
    }

    private static GuestStateMask FullState =>
        new(uint.MaxValue, uint.MaxValue, byte.MaxValue, true, true, true, true, byte.MaxValue, byte.MaxValue);
}
