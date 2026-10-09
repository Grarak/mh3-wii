using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

[Flags]
public enum GuestCallBoundaryFlags
{
    None = 0,
    RequiresCompleteContext = 1 << 0,
    CanSuspend = 1 << 1,
    CanSwitchThreads = 1 << 2,
    InvokesGuestCode = 1 << 3
}

public sealed record GuestAbiContract(
    uint GprReadBeforeWriteMask,
    uint GprPossibleWriteMask,
    uint GprReturnMask,
    uint FprReadBeforeWriteMask,
    uint FprPossibleWriteMask,
    uint FprReturnMask,
    byte CrReadBeforeWriteMask,
    byte CrPossibleWriteMask,
    bool ReadsXerBeforeWrite,
    bool MayWriteXer,
    bool ReadsCtrBeforeWrite,
    bool MayWriteCtr,
    bool ReadsLrBeforeWrite,
    bool MayWriteLr,
    GuestCallBoundaryFlags BoundaryFlags,
    IReadOnlyList<uint> DirectCallTargets)
{
    public bool ReadsFpscrBeforeWrite { get; init; }
    public bool MayWriteFpscr { get; init; }
    public byte GqrReadBeforeWriteMask { get; init; }
    public byte GqrPossibleWriteMask { get; init; }
    public byte HidReadBeforeWriteMask { get; init; }
    public byte HidPossibleWriteMask { get; init; }
    public uint GprDefiniteWriteMask { get; init; }
    public uint FprDefiniteWriteMask { get; init; }
    public byte CrDefiniteWriteMask { get; init; }
    public bool DefinitelyWritesXer { get; init; }
    public bool DefinitelyWritesCtr { get; init; }
    public bool DefinitelyWritesLr { get; init; }
    public bool DefinitelyWritesFpscr { get; init; }
    public byte GqrDefiniteWriteMask { get; init; }
    public byte HidDefiniteWriteMask { get; init; }

    public bool HasFullSynchronizationFence =>
        (BoundaryFlags & GuestCallBoundaryFlags.RequiresCompleteContext) != 0;
}

public static class GuestAbiContractAnalyzer
{
    private readonly record struct RegisterSet(
        uint Gpr, uint Fpr, byte Cr, bool Xer, bool Ctr, bool Lr, bool Fpscr, byte Gqr, byte Hid)
    {
        public static RegisterSet Empty => default;
        public RegisterSet Union(RegisterSet other) =>
            new(Gpr | other.Gpr, Fpr | other.Fpr, (byte)(Cr | other.Cr),
                Xer || other.Xer, Ctr || other.Ctr, Lr || other.Lr,
                Fpscr || other.Fpscr, (byte)(Gqr | other.Gqr), (byte)(Hid | other.Hid));
        public RegisterSet Intersect(RegisterSet other) =>
            new(Gpr & other.Gpr, Fpr & other.Fpr, (byte)(Cr & other.Cr),
                Xer && other.Xer, Ctr && other.Ctr, Lr && other.Lr,
                Fpscr && other.Fpscr, (byte)(Gqr & other.Gqr), (byte)(Hid & other.Hid));
        public RegisterSet Except(RegisterSet other) =>
            new(Gpr & ~other.Gpr, Fpr & ~other.Fpr, (byte)(Cr & ~other.Cr),
                Xer && !other.Xer, Ctr && !other.Ctr, Lr && !other.Lr,
                Fpscr && !other.Fpscr, (byte)(Gqr & ~other.Gqr), (byte)(Hid & ~other.Hid));
    }

    public static GuestAbiContract Analyze(
        IrFunction function,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts = null)
    {
        // Block labels are replaced by ordinals for the duration of the
        // analysis: the fixpoint below runs on every function several times per
        // interprocedural round, and string-keyed dictionaries dominated it.
        var blockList = function.Blocks;
        var blockCount = blockList.Count;
        var ordinals = new Dictionary<string, int>(blockCount, StringComparer.Ordinal);
        for (var index = 0; index < blockCount; ++index)
        {
            ordinals.Add(blockList[index].Label, index);
        }

        var predecessors = new List<int>[blockCount];
        for (var index = 0; index < blockCount; ++index) predecessors[index] = new List<int>();
        for (var index = 0; index < blockCount; ++index)
        {
            var successorCount = Successors(blockList[index], out var first, out var second);
            if (successorCount >= 1 && ordinals.TryGetValue(first, out var firstOrdinal))
                predecessors[firstOrdinal].Add(index);
            if (successorCount == 2 && ordinals.TryGetValue(second, out var secondOrdinal))
                predecessors[secondOrdinal].Add(index);
        }

        var definiteWritesByBlock = new RegisterSet[blockCount];
        for (var index = 0; index < blockCount; ++index)
        {
            var instructions = blockList[index].Instructions;
            var set = RegisterSet.Empty;
            for (var position = 0; position < instructions.Count; ++position)
            {
                Writes(instructions[position], calleeContracts, out _, out var definiteWrite);
                set = set.Union(definiteWrite);
            }
            definiteWritesByBlock[index] = set;
        }

        var definiteIn = new RegisterSet[blockCount];
        var definiteOut = new RegisterSet[blockCount];

        bool changed;
        do
        {
            changed = false;
            for (var index = 0; index < blockCount; ++index)
            {
                var blockPredecessors = predecessors[index];
                RegisterSet incoming;
                if (blockList[index].Label == function.EntryLabel || blockPredecessors.Count == 0)
                {
                    incoming = RegisterSet.Empty;
                }
                else
                {
                    incoming = definiteOut[blockPredecessors[0]];
                    for (var position = 1; position < blockPredecessors.Count; ++position)
                        incoming = incoming.Intersect(definiteOut[blockPredecessors[position]]);
                }

                var outgoing = incoming.Union(definiteWritesByBlock[index]);
                if (incoming != definiteIn[index] || outgoing != definiteOut[index])
                {
                    definiteIn[index] = incoming;
                    definiteOut[index] = outgoing;
                    changed = true;
                }
            }
        } while (changed);

        var readBeforeWrite = RegisterSet.Empty;
        var possibleWrites = RegisterSet.Empty;
        var directTargets = new SortedSet<uint>();
        var boundaryFlags = GuestCallBoundaryFlags.None;
        for (var index = 0; index < blockCount; ++index)
        {
            var written = definiteIn[index];
            var instructions = blockList[index].Instructions;
            for (var position = 0; position < instructions.Count; ++position)
            {
                var instruction = instructions[position];
                readBeforeWrite = readBeforeWrite.Union(Reads(instruction, calleeContracts).Except(written));
                Writes(instruction, calleeContracts, out var possibleInstructionWrites, out var definiteInstructionWrites);
                possibleWrites = possibleWrites.Union(possibleInstructionWrites);
                written = written.Union(definiteInstructionWrites);
                if (instruction is IrCall call && GuestTargetParser.TryParseAddress(call.Target, out var target))
                {
                    if (!TryGetInlineThunk(target, out _, out _))
                        directTargets.Add(target);
                    if (calleeContracts?.TryGetValue(target, out var callee) == true)
                        boundaryFlags |= callee.BoundaryFlags;
                }
                else if (instruction is IrCall helperCall)
                {
                    boundaryFlags |= GuestHelperEffectCatalog.Analyze(helperCall).BoundaryFlags;
                }
                if (instruction is IrIndirectCall or IrIndirectJump or IrUndefined)
                {
                    boundaryFlags |= GuestCallBoundaryFlags.RequiresCompleteContext;
                }
            }
        }

        var definiteWrites = RegisterSet.Empty;
        var sawExitBlock = false;
        for (var index = 0; index < blockCount; ++index)
        {
            var successorCount = Successors(blockList[index], out var first, out var second);
            if ((successorCount >= 1 && ordinals.ContainsKey(first)) ||
                (successorCount == 2 && ordinals.ContainsKey(second)))
                continue;
            definiteWrites = sawExitBlock ? definiteWrites.Intersect(definiteOut[index]) : definiteOut[index];
            sawExitBlock = true;
        }

        const uint gprReturnMask = (1u << 3) | (1u << 4);
        const uint fprReturnMask = 1u << 1;
        return new GuestAbiContract(
            readBeforeWrite.Gpr,
            possibleWrites.Gpr,
            possibleWrites.Gpr & gprReturnMask,
            readBeforeWrite.Fpr,
            possibleWrites.Fpr,
            possibleWrites.Fpr & fprReturnMask,
            readBeforeWrite.Cr,
            possibleWrites.Cr,
            readBeforeWrite.Xer,
            possibleWrites.Xer,
            readBeforeWrite.Ctr,
            possibleWrites.Ctr,
            readBeforeWrite.Lr,
            possibleWrites.Lr,
            boundaryFlags,
            directTargets.ToArray())
        {
            ReadsFpscrBeforeWrite = readBeforeWrite.Fpscr,
            MayWriteFpscr = possibleWrites.Fpscr,
            GqrReadBeforeWriteMask = readBeforeWrite.Gqr,
            GqrPossibleWriteMask = possibleWrites.Gqr,
            HidReadBeforeWriteMask = readBeforeWrite.Hid,
            HidPossibleWriteMask = possibleWrites.Hid,
            GprDefiniteWriteMask = definiteWrites.Gpr,
            FprDefiniteWriteMask = definiteWrites.Fpr,
            CrDefiniteWriteMask = definiteWrites.Cr,
            DefinitelyWritesXer = definiteWrites.Xer,
            DefinitelyWritesCtr = definiteWrites.Ctr,
            DefinitelyWritesLr = definiteWrites.Lr,
            DefinitelyWritesFpscr = definiteWrites.Fpscr,
            GqrDefiniteWriteMask = definiteWrites.Gqr,
            HidDefiniteWriteMask = definiteWrites.Hid
        };
    }

    /// <summary>
    /// Successor count (0, 1, or 2) and labels for <paramref name="block"/>; count-plus-out-params
    /// keeps the CFG walk allocation-free.
    /// </summary>
    private static int Successors(IrBasicBlock block, out string first, out string second)
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

        first = string.Empty;
        second = string.Empty;
        return 0;
    }

    private static RegisterSet Reads(
        IrInstruction instruction,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts)
    {
        var result = RegisterSet.Empty;
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
                Add(ref result, value.FirstOriginalAddress.Base);
                Add(ref result, value.SecondOriginalAddress.Base);
                AddValue(ref result, value.FirstSource);
                AddValue(ref result, value.SecondSource);
                break;
            case IrCall value:
                if (GuestTargetParser.TryParseAddress(value.Target, out var directTarget) &&
                    calleeContracts?.TryGetValue(directTarget, out var callee) == true)
                {
                    // A decoded call carries the volatile ABI registers as generic IR arguments;
                    // once the callee has a contract, only state it actually consumes is a real read.
                    result = result.Union(ContractReads(callee));
                }
                else if (GuestTargetParser.TryParseAddress(value.Target, out var readThunkAddress) &&
                    TryGetInlineThunk(readThunkAddress, out var readThunk, out var readStart) &&
                    readThunk is InlineThunkKind.SaveGpr or InlineThunkKind.SaveFpr)
                {
                    AddRegisterRangeToTop(ref result, readThunk == InlineThunkKind.SaveGpr, readStart);
                }
                else if (!GuestTargetParser.TryParseAddress(value.Target, out _))
                {
                    foreach (var argument in value.Arguments) AddValue(ref result, argument);
                    result = result.Union(HiddenReads(GuestHelperEffectCatalog.Analyze(value)));
                }
                else
                {
                    // An unresolved direct/native target has no stronger
                    // contract, so its explicitly modelled ABI arguments remain
                    // observable inputs.
                    foreach (var argument in value.Arguments) AddValue(ref result, argument);
                }
                break;
            case IrIndirectCall value:
                AddValue(ref result, value.Target);
                foreach (var argument in value.Arguments) AddValue(ref result, argument);
                result = result.Union(FullState);
                break;
            case IrIndirectJump value:
                AddValue(ref result, value.Target);
                result = result.Union(FullState);
                break;
            case IrSetCrField value:
                AddValue(ref result, value.Left);
                AddValue(ref result, value.Right);
                // Integer comparisons copy XER.SO into the CR field.
                result = result with { Xer = true };
                break;
            case IrPhi value: foreach (var source in value.Sources.Values) Add(ref result, source); break;
            case IrBranch value:
                Add(ref result, value.ConditionRegister);
                result = result.Union(SpecialRegistersInRawExpression(value.ConditionRegister));
                break;
            case IrJumpTable value: Add(ref result, value.Selector); break;
            case IrReturn { Value: { } value }: AddValue(ref result, value); break;
        }
        return result;
    }

    /// <summary>
    /// Classifies both write flavours of an instruction in one pass: <paramref name="definite"/> is
    /// guaranteed, <paramref name="possible"/> is what it may write. Only call cases differ.
    /// </summary>
    private static void Writes(
        IrInstruction instruction,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts,
        out RegisterSet possible,
        out RegisterSet definite)
    {
        var shared = RegisterSet.Empty;
        var possibleOnly = RegisterSet.Empty;
        var definiteOnly = RegisterSet.Empty;
        switch (instruction)
        {
            case IrAssign value: Add(ref shared, value.Destination); break;
            case IrBinary value: Add(ref shared, value.Destination); break;
            case IrLoad value: Add(ref shared, value.Destination); break;
            case IrResolveGuestMemoryRange value: Add(ref shared, value.Destination); break;
            case IrResolvedLoad value: Add(ref shared, value.Destination); break;
            case IrResolvedPsqLoad value: Add(ref shared, value.Destination); break;
            case IrResolvedLoadPair value:
                Add(ref shared, value.FirstDestination);
                Add(ref shared, value.SecondDestination);
                break;
            case IrCall value:
                var helperEffect = !GuestTargetParser.TryParseAddress(value.Target, out _)
                    ? GuestHelperEffectCatalog.Analyze(value)
                    : null;
                var destination = Register(value.Destination);
                // CR helpers return the complete packed CR only because that is
                // the runtime helper ABI. Architecturally they change the fields
                // described by the helper contract, not all eight fields.
                if (!(destination.Cr == byte.MaxValue && helperEffect?.CrWriteMask != 0))
                    shared = shared.Union(destination);
                if (GuestTargetParser.TryParseAddress(value.Target, out var directTarget) &&
                    calleeContracts?.TryGetValue(directTarget, out var callee) == true)
                {
                    possibleOnly = ContractWrites(callee);
                    definiteOnly = ContractDefiniteWrites(callee);
                }
                else if (GuestTargetParser.TryParseAddress(value.Target, out var writeThunkAddress) &&
                    TryGetInlineThunk(writeThunkAddress, out var writeThunk, out var writeStart) &&
                    writeThunk is InlineThunkKind.RestGpr or InlineThunkKind.RestFpr)
                {
                    AddRegisterRangeToTop(ref shared, writeThunk == InlineThunkKind.RestGpr, writeStart);
                }
                else if (helperEffect is not null)
                {
                    // Hidden helper state is a possible write only: the definite
                    // flavour deliberately ignores non-guest call targets.
                    possibleOnly = HiddenWrites(helperEffect);
                }
                break;
            case IrIndirectCall value:
                Add(ref shared, value.Destination);
                shared = shared.Union(FullState);
                break;
            case IrSetCrField value:
                shared = shared with { Cr = (byte)(shared.Cr | (1 << value.FieldIndex)) };
                break;
            case IrPhi value: Add(ref shared, value.Destination); break;
            case IrUndefined:
                shared = FullState;
                break;
        }

        possible = shared.Union(possibleOnly);
        definite = shared.Union(definiteOnly);
    }

    /// <summary>
    /// Unions <c>rN..r31</c> or <c>fN..f31</c>, the register window an inline
    /// save/restore thunk touches.
    /// </summary>
    private static void AddRegisterRangeToTop(ref RegisterSet set, bool general, int startRegister)
    {
        var mask = uint.MaxValue << startRegister;
        set = general
            ? set with { Gpr = set.Gpr | mask }
            : set with { Fpr = set.Fpr | mask };
    }

    private static void Add(ref RegisterSet set, string? name) => set = set.Union(Register(name));

    private static void AddValue(ref RegisterSet set, IrValue value)
    {
        if (value.Kind == "register") Add(ref set, value.RegisterName);
    }

    private static void AddGqr(ref RegisterSet set, uint index)
    {
        // Mirrors Register("gqr{index}"): only the eight architectural
        // quantization registers exist, anything else names nothing.
        if (index <= 7) set = set with { Gqr = (byte)(set.Gqr | (1 << (int)index)) };
    }

    private static RegisterSet HiddenReads(GuestHelperEffect effect) =>
        new(effect.GprReadMask, effect.FprReadMask, effect.CrReadMask,
            effect.ReadsXer, effect.ReadsCtr, effect.ReadsLr, false, 0, 0);

    private static RegisterSet HiddenWrites(GuestHelperEffect effect) =>
        new(effect.GprWriteMask, effect.FprWriteMask, effect.CrWriteMask,
            effect.WritesXer, effect.WritesCtr, effect.WritesLr, false, 0, 0);

    private static RegisterSet ContractReads(GuestAbiContract contract) =>
        new(contract.GprReadBeforeWriteMask, contract.FprReadBeforeWriteMask,
            contract.CrReadBeforeWriteMask, contract.ReadsXerBeforeWrite,
            contract.ReadsCtrBeforeWrite, contract.ReadsLrBeforeWrite,
            contract.ReadsFpscrBeforeWrite, contract.GqrReadBeforeWriteMask,
            contract.HidReadBeforeWriteMask);

    private static RegisterSet ContractWrites(GuestAbiContract contract) =>
        new(contract.GprPossibleWriteMask, contract.FprPossibleWriteMask,
            contract.CrPossibleWriteMask, contract.MayWriteXer,
            contract.MayWriteCtr, contract.MayWriteLr,
            contract.MayWriteFpscr, contract.GqrPossibleWriteMask,
            contract.HidPossibleWriteMask);

    private static RegisterSet ContractDefiniteWrites(GuestAbiContract contract) =>
        new(contract.GprDefiniteWriteMask, contract.FprDefiniteWriteMask,
            contract.CrDefiniteWriteMask, contract.DefinitelyWritesXer,
            contract.DefinitelyWritesCtr, contract.DefinitelyWritesLr,
            contract.DefinitelyWritesFpscr, contract.GqrDefiniteWriteMask,
            contract.HidDefiniteWriteMask);

    private static RegisterSet FullState =>
        new(uint.MaxValue, uint.MaxValue, byte.MaxValue, true, true, true, true, byte.MaxValue, byte.MaxValue);

    private static RegisterSet Register(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return RegisterSet.Empty;
        // Classification runs on a span: an SSA name such as "r3_12" used to
        // cost two substrings and a LINQ digit scan before anything was decided.
        var baseName = SsaBaseName(name.AsSpan());
        if (baseName.Length >= 2 && baseName[0] == 'r' && int.TryParse(baseName[1..], out var gpr) && gpr is >= 0 and < 32)
            return RegisterSet.Empty with { Gpr = 1u << gpr };
        if (baseName.Length >= 2 && baseName[0] == 'f' && int.TryParse(baseName[1..], out var fpr) && fpr is >= 0 and < 32)
            return RegisterSet.Empty with { Fpr = 1u << fpr };
        if (baseName.Length == 3 && baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
            baseName[2] is >= '0' and <= '7')
            return RegisterSet.Empty with { Cr = (byte)(1 << (baseName[2] - '0')) };
        if (baseName.Length is >= 4 and <= 5 && baseName.StartsWith("crb", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(baseName[3..], out var crBit) && crBit is >= 0 and < 32)
            return RegisterSet.Empty with { Cr = (byte)(1 << (crBit / 4)) };
        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase))
            return RegisterSet.Empty with { Cr = byte.MaxValue };
        if (baseName.Equals("xer", StringComparison.OrdinalIgnoreCase))
            return RegisterSet.Empty with { Xer = true };
        if (baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase))
            return RegisterSet.Empty with { Ctr = true };
        if (baseName.Equals("lr", StringComparison.OrdinalIgnoreCase))
            return RegisterSet.Empty with { Lr = true };
        if (baseName.Equals("fpscr", StringComparison.OrdinalIgnoreCase))
            return RegisterSet.Empty with { Fpscr = true };
        if (baseName.Length == 4 && baseName.StartsWith("gqr", StringComparison.OrdinalIgnoreCase) &&
            baseName[3] is >= '0' and <= '7')
            return RegisterSet.Empty with { Gqr = (byte)(1 << (baseName[3] - '0')) };
        if (baseName.Length == 4 && baseName.StartsWith("hid", StringComparison.OrdinalIgnoreCase) &&
            baseName[3] is >= '0' and <= '2')
            return RegisterSet.Empty with { Hid = (byte)(1 << (baseName[3] - '0')) };
        return RegisterSet.Empty;
    }

    /// <summary>
    /// Drops a trailing SSA version suffix: everything after the first
    /// underscore is removed when it consists solely of digits.
    /// </summary>
    private static ReadOnlySpan<char> SsaBaseName(ReadOnlySpan<char> name)
    {
        var underscore = name.IndexOf('_');
        if (underscore <= 0) return name;
        for (var index = underscore + 1; index < name.Length; ++index)
        {
            if (!char.IsDigit(name[index])) return name;
        }

        return name[..underscore];
    }

    private static RegisterSet SpecialRegistersInRawExpression(string expression) =>
        new(
            0,
            0,
            expression.Contains("ctx->cr", StringComparison.Ordinal) ||
            expression.Contains("GetCRBit(ctx", StringComparison.Ordinal) ? byte.MaxValue : (byte)0,
            expression.Contains("ctx->xer", StringComparison.Ordinal),
            expression.Contains("ctx->ctr", StringComparison.Ordinal),
            expression.Contains("ctx->lr", StringComparison.Ordinal),
            expression.Contains("ctx->fpscr", StringComparison.Ordinal),
            0,
            (byte)((expression.Contains("ctx->hid0", StringComparison.Ordinal) ? 1 : 0) |
                   (expression.Contains("ctx->hid1", StringComparison.Ordinal) ? 2 : 0) |
                   (expression.Contains("ctx->hid2", StringComparison.Ordinal) ? 4 : 0)));

    private enum InlineThunkKind { SaveGpr, RestGpr, SaveFpr, RestFpr }

    private static bool TryGetInlineThunk(uint address, out InlineThunkKind kind, out int startRegister)
    {
        var thunks = GuestSaveRestoreThunks.Current;
        if (TryRange(address, thunks.SaveGpr, out startRegister)) { kind = InlineThunkKind.SaveGpr; return true; }
        if (TryRange(address, thunks.RestGpr, out startRegister)) { kind = InlineThunkKind.RestGpr; return true; }
        if (TryRange(address, thunks.SaveFpr, out startRegister)) { kind = InlineThunkKind.SaveFpr; return true; }
        if (TryRange(address, thunks.RestFpr, out startRegister)) { kind = InlineThunkKind.RestFpr; return true; }
        kind = default;
        startRegister = -1;
        return false;

        static bool TryRange(uint candidate, GuestSaveRestoreThunkRange? range, out int start)
        {
            if (range is null)
            {
                start = -1;
                return false;
            }

            var (baseAddress, firstRegister, lastStartRegister) = range;
            var lastAddress = baseAddress + checked((uint)((lastStartRegister - firstRegister) * 4));
            if (candidate < baseAddress || candidate > lastAddress || ((candidate - baseAddress) & 3u) != 0)
            {
                start = -1;
                return false;
            }
            start = firstRegister + checked((int)((candidate - baseAddress) / 4));
            return true;
        }
    }
}
