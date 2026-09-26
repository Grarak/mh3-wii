using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private sealed record LeafRegisterCache(
        SortedSet<int> Gprs,
        SortedSet<int> Fprs,
        SortedSet<int> WrittenGprs,
        SortedSet<int> WrittenFprs,
        bool CacheSpecialRegisters = true)
    {
        public static LeafRegisterCache Empty { get; } = new(
            new SortedSet<int>(),
            new SortedSet<int>(),
            new SortedSet<int>(),
            new SortedSet<int>());

        public bool IsEmpty => Gprs.Count == 0 && Fprs.Count == 0;
        public bool HasFlushes => WrittenGprs.Count != 0 || WrittenFprs.Count != 0;
    }

    private sealed record LeafRegisterCacheInitializers(SortedSet<int> Gprs, SortedSet<int> Fprs)
    {
        public static LeafRegisterCacheInitializers Empty { get; } = new(new SortedSet<int>(), new SortedSet<int>());
    }

    private sealed record RegisterLiveness(
        Dictionary<string, HashSet<string>> In);

    /// <summary>
    /// Helpers that touch cached architectural state via CpuContext instead of explicit args.
    /// The leaf register cache can't see that traffic (e.g. cached_cr would go stale vs PPC_Fcmp),
    /// so functions calling them are excluded; the residency path syncs them via inline expansion instead.
    /// </summary>
    private static readonly HashSet<string> TlsContextStateHelpers = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_Fcmp", "PPC_PsCmpo0", "PPC_PsCmpo1", "PPC_PsCmpu0", "PPC_PsCmpu1",
        "PPC_Stwcx", "PPC_Mcrxr", "PPC_Mcrfs",
        "PPC_Lswi", "PPC_Lswx", "PPC_Stswi", "PPC_Stswx",
        "OSSystemCall"
    };

    private static bool FunctionEligibleForLeafRegisterCache(IrFunction func)
    {
        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrIndirectCall:
                    case IrIndirectJump:
                    case IrJumpTable:
                        return false;

                    case IrCall call:
                        if (TryParseAddress(call.Target, out var address))
                        {
                            if (TryGetInlineGuestThunkSpec(address, out _))
                            {
                                continue;
                            }

                            return false;
                        }

                        if (IsGuestCallTarget(call.Target))
                        {
                            return false;
                        }

                        if (TlsContextStateHelpers.Contains(call.Target))
                        {
                            return false;
                        }
                        break;
                }
            }
        }

        return true;
    }

    private static LeafRegisterCache CollectLeafRegisterCacheRegisters(
        IrFunction func,
        IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null)
    {
        var gprs = new SortedSet<int>();
        var fprs = new SortedSet<int>();
        var writtenGprs = new SortedSet<int>();
        var writtenFprs = new SortedSet<int>();

        void AddRegister(string? registerName, bool written = false)
        {
            if (string.IsNullOrWhiteSpace(registerName))
            {
                return;
            }

            var baseName = GetRegisterBaseName(registerName);
            if (TryParseGprRegisterIndex(baseName, out var gpr))
            {
                gprs.Add(gpr);
                if (written)
                {
                    writtenGprs.Add(gpr);
                }

                return;
            }

            if (TryParseFloatRegisterIndex(baseName, out var fpr))
            {
                fprs.Add(fpr);
                if (written)
                {
                    writtenFprs.Add(fpr);
                }
            }
        }

        void AddValue(IrValue value)
        {
            if (value.Kind == "register")
            {
                AddRegister(value.RegisterName);
            }
        }

        void AddAddress(IrAddress address) => AddRegister(address.Base);

        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrAssign assign:
                        AddRegister(assign.Destination, written: true);
                        AddValue(assign.Value);
                        break;

                    case IrBinary binary:
                        AddRegister(binary.Destination, written: true);
                        AddValue(binary.Left);
                        AddValue(binary.Right);
                        break;

                    case IrLoad load:
                        AddRegister(load.Destination, written: true);
                        AddAddress(load.Address);
                        break;

                    case IrStore store:
                        AddAddress(store.Address);
                        AddValue(store.Source);
                        break;

                    case IrResolveGuestMemoryRange resolve:
                        AddValue(resolve.Base);
                        break;

                    case IrResolvedLoad load:
                        AddRegister(load.Destination, written: true);
                        AddAddress(load.OriginalAddress);
                        break;

                    case IrResolvedStore store:
                        AddAddress(store.OriginalAddress);
                        AddValue(store.Source);
                        break;

                    case IrResolvedPsqLoad load:
                        AddRegister(load.Destination, written: true);
                        AddValue(load.OriginalAddress);
                        break;


                    case IrResolvedPsqStore store:
                        AddValue(store.OriginalAddress);
                        AddValue(store.Source);
                        break;

                    case IrResolvedLoadPair pair:
                        AddRegister(pair.FirstDestination, written: true);
                        AddRegister(pair.SecondDestination, written: true);
                        AddAddress(pair.FirstOriginalAddress);
                        AddAddress(pair.SecondOriginalAddress);
                        break;

                    case IrResolvedStorePair pair:
                        AddAddress(pair.FirstOriginalAddress);
                        AddAddress(pair.SecondOriginalAddress);
                        AddValue(pair.FirstSource);
                        AddValue(pair.SecondSource);
                        break;

                    case IrCall call:
                        AddRegister(call.Destination, written: true);

                        if (TryParseAddress(call.Target, out var address) &&
                            TryGetInlineGuestThunkSpec(address, out _))
                        {
                            AddInlineGuestThunkReads(call);
                            AddInlineGuestThunkWrites(call);
                            break;
                        }

                        if (IsGuestCallTarget(call.Target))
                        {
                            var (readMask, writeMask) = GuestCallGprEffects(call, guestAbiContracts);
                            for (var gpr = 0; gpr < 32; ++gpr)
                            {
                                if ((readMask & (1u << gpr)) != 0) AddRegister(GprRegisterName(gpr));
                                if ((writeMask & (1u << gpr)) != 0) AddRegister(GprRegisterName(gpr), written: true);
                            }
                            var (fprReadMask, fprWriteMask) = GuestCallFprEffects(call, guestAbiContracts);
                            for (var fpr = 0; fpr < 32; ++fpr)
                            {
                                if ((fprReadMask & (1u << fpr)) != 0) AddRegister(FprRegisterName(fpr));
                                if ((fprWriteMask & (1u << fpr)) != 0) AddRegister(FprRegisterName(fpr), written: true);
                            }
                            break;
                        }

                        foreach (var argument in call.Arguments)
                        {
                            AddValue(argument);
                        }
                        var helperEffect = AnalyzeGuestHelperEffect(call);
                        for (var gpr = 0; gpr < 32; ++gpr)
                        {
                            if ((helperEffect.GprReadMask & (1u << gpr)) != 0) AddRegister(GprRegisterName(gpr));
                            if ((helperEffect.GprWriteMask & (1u << gpr)) != 0) AddRegister(GprRegisterName(gpr), written: true);
                        }
                        for (var fpr = 0; fpr < 32; ++fpr)
                        {
                            if ((helperEffect.FprReadMask & (1u << fpr)) != 0) AddRegister(FprRegisterName(fpr));
                            if ((helperEffect.FprWriteMask & (1u << fpr)) != 0) AddRegister(FprRegisterName(fpr), written: true);
                        }
                        break;

                    case IrIndirectCall indirectCall:
                        AddRegister(indirectCall.Destination, written: true);
                        AddValue(indirectCall.Target);
                        foreach (var argument in indirectCall.Arguments)
                        {
                            AddValue(argument);
                        }

                        break;

                    case IrIndirectJump indirectJump:
                        AddValue(indirectJump.Target);
                        break;

                    case IrPhi phi:
                        AddRegister(phi.Destination, written: true);
                        foreach (var source in phi.Sources.Values)
                        {
                            AddRegister(source);
                        }
                        break;

                    case IrReturn ret:
                        if (ret.Value is { } value)
                        {
                            AddValue(value);
                        }
                        break;

                    case IrBranch branch when IsCpuRegister(branch.ConditionRegister):
                        AddRegister(branch.ConditionRegister);
                        break;

                    case IrJumpTable table:
                        AddRegister(table.Selector);
                        break;

                    case IrSetCrField setCr:
                        AddValue(setCr.Left);
                        AddValue(setCr.Right);
                        break;
                }
            }
        }

        return new LeafRegisterCache(gprs, fprs, writtenGprs, writtenFprs);

        void AddInlineGuestThunkWrites(IrCall call)
        {
            if (!TryParseAddress(call.Target, out var address) ||
                !TryGetInlineGuestThunkSpec(address, out var spec))
            {
                return;
            }

            switch (spec.Kind)
            {
                case InlineGuestThunkKind.RestGpr:
                    for (var reg = spec.StartRegister; reg <= 31; reg++)
                    {
                        AddRegister(GprRegisterName(reg), written: true);
                    }
                    break;

                case InlineGuestThunkKind.RestFpr:
                    for (var reg = spec.StartRegister; reg <= 31; reg++)
                    {
                        AddRegister(FprRegisterName(reg), written: true);
                    }
                    break;
            }
        }

        void AddInlineGuestThunkReads(IrCall call)
        {
            if (!TryParseAddress(call.Target, out var address) ||
                !TryGetInlineGuestThunkSpec(address, out var spec))
            {
                return;
            }

            switch (spec.Kind)
            {
                case InlineGuestThunkKind.SaveGpr:
                    for (var reg = spec.StartRegister; reg <= 31; reg++)
                    {
                        AddRegister(GprRegisterName(reg));
                    }
                    break;

                case InlineGuestThunkKind.SaveFpr:
                    for (var reg = spec.StartRegister; reg <= 31; reg++)
                    {
                        AddRegister(FprRegisterName(reg));
                    }
                    break;
            }
        }
    }

    private static LeafRegisterCacheInitializers BuildLeafRegisterCacheInitializers(
        IrFunction func,
        IrCfg cfg,
        LeafRegisterCache registers,
        IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null)
    {
        if (registers.IsEmpty)
        {
            return LeafRegisterCacheInitializers.Empty;
        }

        var liveness = ComputeRegisterLiveness(func, cfg, registers, guestAbiProvider, guestAbiContracts);
        if (!liveness.In.TryGetValue(func.EntryLabel, out var entryLive))
        {
            return LeafRegisterCacheInitializers.Empty;
        }

        var gprs = new SortedSet<int>(registers.Gprs
            .Where(gpr => entryLive.Contains(GprRegisterName(gpr))));
        var fprs = new SortedSet<int>(registers.Fprs
            .Where(fpr => entryLive.Contains(FprRegisterName(fpr))));
        return new LeafRegisterCacheInitializers(gprs, fprs);
    }

    private static RegisterLiveness ComputeRegisterLiveness(
        IrFunction func,
        IrCfg cfg,
        LeafRegisterCache registers,
        IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var liveIn = func.Blocks.ToDictionary(
            static block => block.Label,
            _ => new HashSet<string>(comparer),
            comparer);

        // Materialized once: Enumerable.Reverse buffers the whole block list on
        // every enumeration, and this loop enumerates it per fixpoint iteration.
        var reversedBlocks = func.Blocks.Reverse().ToArray();
        // Scratch sets, so a converged iteration (the common case, and always
        // the last one) allocates nothing at all. A set is only copied into the
        // result maps when it actually differs from the recorded state.
        var newOut = new HashSet<string>(comparer);
        var newIn = new HashSet<string>(comparer);

        var changed = true;
        while (changed)
        {
            changed = false;

            foreach (var block in reversedBlocks)
            {
                newOut.Clear();
                foreach (var successor in cfg.Successors(block.Label))
                {
                    if (liveIn.TryGetValue(successor, out var successorLiveIn))
                    {
                        newOut.UnionWith(successorLiveIn);
                    }
                }

                newIn.Clear();
                newIn.UnionWith(newOut);
                for (var i = block.Instructions.Count - 1; i >= 0; i--)
                {
                    ApplyLivenessTransfer(block.Instructions[i], newIn, registers, guestAbiProvider, guestAbiContracts);
                }

                if (!newIn.SetEquals(liveIn[block.Label]))
                {
                    liveIn[block.Label] = new HashSet<string>(newIn, comparer);
                    changed = true;
                }
            }
        }

        return new RegisterLiveness(liveIn);
    }

    private static void ApplyLivenessTransfer(
        IrInstruction instruction,
        HashSet<string> live,
        LeafRegisterCache registers,
        IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts)
    {
        if (instruction is IrPhi phi)
        {
            // Phi operands are edge uses only when the phi result is live.
            // Treating every source as unconditional kept dead architectural
            // FPRs live around loops and forced representation conversions.
            var destination = NormalizeLivenessRegisterName(phi.Destination);
            if (live.Remove(destination))
            {
                foreach (var source in phi.Sources.Values)
                    live.Add(NormalizeLivenessRegisterName(source));
            }
            return;
        }

        foreach (var def in InstructionRegisterDefsForLiveness(instruction, guestAbiProvider, guestAbiContracts))
        {
            live.Remove(def);
        }

        foreach (var use in InstructionRegisterUsesForLiveness(instruction, registers, guestAbiContracts))
        {
            live.Add(use);
        }
    }

    private static IEnumerable<string> InstructionRegisterUsesForLiveness(
        IrInstruction instruction,
        LeafRegisterCache registers,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts)
    {
        static IEnumerable<IrValue> Values(params IrValue[] values) => values;

        IEnumerable<string> ReturnFlushUses()
        {
            foreach (var gpr in registers.WrittenGprs)
            {
                yield return GprRegisterName(gpr);
            }

            foreach (var fpr in registers.WrittenFprs)
            {
                yield return FprRegisterName(fpr);
            }
        }

        switch (instruction)
        {
            case IrAssign assign:
                foreach (var use in RegisterUsesFromValues(Values(assign.Value)))
                {
                    yield return use;
                }
                break;

            case IrBinary binary:
                foreach (var use in RegisterUsesFromValues(Values(binary.Left, binary.Right)))
                {
                    yield return use;
                }
                break;

            case IrLoad load:
                yield return NormalizeLivenessRegisterName(load.Address.Base);
                break;

            case IrStore store:
                yield return NormalizeLivenessRegisterName(store.Address.Base);
                foreach (var use in RegisterUsesFromValues(Values(store.Source)))
                {
                    yield return use;
                }
                break;

            case IrResolveGuestMemoryRange resolve:
                foreach (var use in RegisterUsesFromValues(Values(resolve.Base)))
                    yield return use;
                break;

            case IrResolvedLoad load:
                yield return NormalizeLivenessRegisterName(load.OriginalAddress.Base);
                break;

            case IrResolvedStore store:
                yield return NormalizeLivenessRegisterName(store.OriginalAddress.Base);
                foreach (var use in RegisterUsesFromValues(Values(store.Source)))
                    yield return use;
                break;

            case IrCall call:
                foreach (var use in InlineGuestThunkRegisterUses(call))
                {
                    yield return use;
                }

                foreach (var use in RegisterUsesFromValues(call.Arguments))
                {
                    yield return use;
                }

                if (IsGuestCallTarget(call.Target))
                {
                    var (readMask, _) = GuestCallGprEffects(call, guestAbiContracts);
                    for (var gpr = 0; gpr < 32; ++gpr)
                        if ((readMask & (1u << gpr)) != 0) yield return GprRegisterName(gpr);
                    var (fprReadMask, _) = GuestCallFprEffects(call, guestAbiContracts);
                    for (var fpr = 0; fpr < 32; ++fpr)
                        if ((fprReadMask & (1u << fpr)) != 0) yield return FprRegisterName(fpr);
                }
                else if (!TryParseAddress(call.Target, out _))
                {
                    var helperEffect = AnalyzeGuestHelperEffect(call);
                    for (var gpr = 0; gpr < 32; ++gpr)
                        if ((helperEffect.GprReadMask & (1u << gpr)) != 0) yield return GprRegisterName(gpr);
                }

                break;

            case IrIndirectCall indirectCall:
                foreach (var use in RegisterUsesFromValues(Values(indirectCall.Target)) )
                {
                    yield return use;
                }

                foreach (var use in RegisterUsesFromValues(indirectCall.Arguments))
                {
                    yield return use;
                }

                break;

            case IrIndirectJump indirectJump:
                foreach (var use in RegisterUsesFromValues(Values(indirectJump.Target)))
                {
                    yield return use;
                }
                break;

            case IrPhi phi:
                foreach (var source in phi.Sources.Values)
                {
                    yield return NormalizeLivenessRegisterName(source);
                }
                break;

            case IrResolvedPsqStore store:
                foreach (var use in RegisterUsesFromValues(Values(store.OriginalAddress, store.Source)))
                    yield return use;
                break;

            case IrResolvedPsqLoad load:
                foreach (var use in RegisterUsesFromValues(Values(load.OriginalAddress)))
                    yield return use;
                break;


            case IrResolvedLoadPair pair:
                yield return NormalizeLivenessRegisterName(pair.FirstOriginalAddress.Base);
                yield return NormalizeLivenessRegisterName(pair.SecondOriginalAddress.Base);
                break;

            case IrResolvedStorePair pair:
                yield return NormalizeLivenessRegisterName(pair.FirstOriginalAddress.Base);
                yield return NormalizeLivenessRegisterName(pair.SecondOriginalAddress.Base);
                foreach (var use in RegisterUsesFromValues(Values(pair.FirstSource, pair.SecondSource)))
                    yield return use;
                break;

            case IrReturn ret:
                if (ret.Value is { } value)
                {
                    foreach (var use in RegisterUsesFromValues(Values(value)))
                    {
                        yield return use;
                    }
                }

                foreach (var use in ReturnFlushUses())
                {
                    yield return use;
                }
                break;

            case IrBranch branch:
                if (IsCpuRegister(branch.ConditionRegister))
                {
                    yield return NormalizeLivenessRegisterName(branch.ConditionRegister);
                }
                break;

            case IrJumpTable table:
                if (!string.IsNullOrWhiteSpace(table.Selector))
                {
                    yield return NormalizeLivenessRegisterName(table.Selector);
                }
                break;

            case IrSetCrField setCr:
                foreach (var use in RegisterUsesFromValues(Values(setCr.Left, setCr.Right)))
                {
                    yield return use;
                }
                break;
        }
    }

    private static IEnumerable<string> InstructionRegisterDefsForLiveness(
        IrInstruction instruction,
        IGuestFunctionAbiProvider guestAbiProvider,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts)
    {
        switch (instruction)
        {
            case IrAssign assign:
                yield return NormalizeLivenessRegisterName(assign.Destination);
                break;

            case IrBinary binary:
                yield return NormalizeLivenessRegisterName(binary.Destination);
                break;

            case IrLoad load:
                yield return NormalizeLivenessRegisterName(load.Destination);
                break;

            case IrResolveGuestMemoryRange resolve:
                yield return NormalizeLivenessRegisterName(resolve.Destination);
                break;

            case IrResolvedLoad load:
                yield return NormalizeLivenessRegisterName(load.Destination);
                break;

            case IrCall call:
                if (!string.IsNullOrWhiteSpace(call.Destination))
                {
                    yield return NormalizeLivenessRegisterName(call.Destination);
                }

                foreach (var def in InlineGuestThunkRegisterDefs(call))
                {
                    yield return def;
                }

                if (IsGuestCallTarget(call.Target))
                {
                    var (_, writeMask) = GuestCallGprEffects(call, guestAbiContracts);
                    for (var gpr = 0; gpr < 32; ++gpr)
                        if ((writeMask & (1u << gpr)) != 0) yield return GprRegisterName(gpr);
                    var (_, fprWriteMask) = GuestCallFprEffects(call, guestAbiContracts);
                    for (var fpr = 0; fpr < 32; ++fpr)
                        if ((fprWriteMask & (1u << fpr)) != 0) yield return FprRegisterName(fpr);
                }
                else if (!TryParseAddress(call.Target, out _))
                {
                    var helperEffect = AnalyzeGuestHelperEffect(call);
                    for (var gpr = 0; gpr < 32; ++gpr)
                        if ((helperEffect.GprWriteMask & (1u << gpr)) != 0) yield return GprRegisterName(gpr);
                }

                break;

            case IrIndirectCall indirectCall:
                if (!string.IsNullOrWhiteSpace(indirectCall.Destination))
                {
                    yield return NormalizeLivenessRegisterName(indirectCall.Destination);
                }
                break;

            case IrPhi phi:
                yield return NormalizeLivenessRegisterName(phi.Destination);
                break;

            case IrResolvedPsqLoad load:
                yield return NormalizeLivenessRegisterName(load.Destination);
                break;
            case IrResolvedLoadPair pair:
                yield return NormalizeLivenessRegisterName(pair.FirstDestination);
                yield return NormalizeLivenessRegisterName(pair.SecondDestination);
                break;
        }
    }

    private static IEnumerable<string> RegisterUsesFromValues(IEnumerable<IrValue> values)
    {
        foreach (var value in values)
        {
            if (value.Kind == "register" && !string.IsNullOrWhiteSpace(value.RegisterName))
            {
                yield return NormalizeLivenessRegisterName(value.RegisterName);
            }
        }
    }

    private static (uint ReadMask, uint WriteMask) GuestCallGprEffects(
        IrCall call,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts)
    {
        if (TryParseAddress(call.Target, out var address) &&
            TryGetInlineGuestThunkSpec(address, out _))
            return (0, 0);

        if (TryParseAddress(call.Target, out address) &&
            guestAbiContracts is not null &&
            guestAbiContracts.TryGetValue(address, out var contract) &&
            !contract.HasFullSynchronizationFence)
            return (contract.GprReadBeforeWriteMask, contract.GprPossibleWriteMask);

        // Unknown, indirect-capable, native, or full-context guest boundaries
        // observe and may change the complete architectural GPR file.
        return (uint.MaxValue, uint.MaxValue);
    }

    private static (uint ReadMask, uint WriteMask) GuestCallFprEffects(
        IrCall call,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts)
    {
        if (TryParseAddress(call.Target, out var address) &&
            TryGetInlineGuestThunkSpec(address, out _))
            return (0, 0);

        if (TryParseAddress(call.Target, out address) &&
            guestAbiContracts is not null &&
            guestAbiContracts.TryGetValue(address, out var contract) &&
            !contract.HasFullSynchronizationFence)
            return (contract.FprReadBeforeWriteMask, contract.FprPossibleWriteMask);

        return (uint.MaxValue, uint.MaxValue);
    }

    private static IEnumerable<string> InlineGuestThunkRegisterDefs(IrCall call)
    {
        if (!TryParseAddress(call.Target, out var address) ||
            !TryGetInlineGuestThunkSpec(address, out var spec))
        {
            yield break;
        }

        switch (spec.Kind)
        {
            case InlineGuestThunkKind.RestGpr:
                for (var reg = spec.StartRegister; reg <= 31; reg++)
                {
                    yield return GprRegisterName(reg);
                }
                break;

            case InlineGuestThunkKind.RestFpr:
                for (var reg = spec.StartRegister; reg <= 31; reg++)
                {
                    yield return FprRegisterName(reg);
                }
                break;
        }
    }

    private static IEnumerable<string> InlineGuestThunkRegisterUses(IrCall call)
    {
        if (!TryParseAddress(call.Target, out var address) ||
            !TryGetInlineGuestThunkSpec(address, out var spec))
        {
            yield break;
        }

        switch (spec.Kind)
        {
            case InlineGuestThunkKind.SaveGpr:
                for (var reg = spec.StartRegister; reg <= 31; reg++)
                {
                    yield return GprRegisterName(reg);
                }
                break;

            case InlineGuestThunkKind.SaveFpr:
                for (var reg = spec.StartRegister; reg <= 31; reg++)
                {
                    yield return FprRegisterName(reg);
                }
                break;
        }
    }

    private static string ApplyLeafRegisterCache(
        string code,
        string cxxName,
        LeafRegisterCache registers,
        LeafRegisterCacheInitializers initializers)
    {
        var cacheCr = false;
        var cacheXer = false;
        var cacheCtr = false;
        var writeCr = false;
        var writeXer = false;
        var writeCtr = false;
        if (registers.CacheSpecialRegisters)
        {
            // GetCRBit(ctx, field, bit) is raw C++ text, not a register operand, so it must be
            // listed explicitly: a body that only reads CR through it still needs cached_cr to
            // exist, and one that also writes CR must not leave that read on stale ctx->cr.
            cacheCr = code.Contains("cached_cr", StringComparison.Ordinal) ||
                      code.Contains("ctx->cr", StringComparison.Ordinal) ||
                      code.Contains("SetCR(ctx,", StringComparison.Ordinal) ||
                      code.Contains("SetCRFloat(ctx,", StringComparison.Ordinal) ||
                      code.Contains("GetCRBit(ctx,", StringComparison.Ordinal);
            cacheXer = code.Contains("cached_xer", StringComparison.Ordinal) ||
                       code.Contains("ctx->xer", StringComparison.Ordinal) ||
                       code.Contains("SetCR(ctx,", StringComparison.Ordinal);
            cacheCtr = code.Contains("cached_ctr", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr", StringComparison.Ordinal);
            writeCr = code.Contains("SetCR(ctx,", StringComparison.Ordinal) ||
                      code.Contains("SetCRFloat(ctx,", StringComparison.Ordinal) ||
                      code.Contains("ctx->cr =", StringComparison.Ordinal) ||
                      code.Contains("ctx->cr |=", StringComparison.Ordinal) ||
                      code.Contains("ctx->cr &=", StringComparison.Ordinal) ||
                      code.Contains("ctx->cr ^=", StringComparison.Ordinal);
            writeXer = code.Contains("ctx->xer =", StringComparison.Ordinal) ||
                       code.Contains("ctx->xer |=", StringComparison.Ordinal) ||
                       code.Contains("ctx->xer &=", StringComparison.Ordinal) ||
                       code.Contains("ctx->xer ^=", StringComparison.Ordinal);
            writeCtr = code.Contains("ctx->ctr =", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr +=", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr -=", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr |=", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr &=", StringComparison.Ordinal) ||
                       code.Contains("ctx->ctr ^=", StringComparison.Ordinal);
            if (cacheCr && cacheXer)
                code = code.Replace("SetCR(ctx,", "SetCRResident(cached_cr, cached_xer,", StringComparison.Ordinal);
            if (cacheCr)
            {
                code = code.Replace("SetCRFloat(ctx,", "SetCRFloatResident(cached_cr,", StringComparison.Ordinal);
                code = code.Replace("GetCRBit(ctx,", "GetCRBitResident(cached_cr,", StringComparison.Ordinal);
                code = code.Replace("ctx->cr", "cached_cr", StringComparison.Ordinal);
            }
            if (cacheXer)
                code = code.Replace("ctx->xer", "cached_xer", StringComparison.Ordinal);
            if (cacheCtr)
                code = code.Replace("ctx->ctr", "cached_ctr", StringComparison.Ordinal);
        }
        foreach (var gpr in registers.Gprs)
        {
            code = code.Replace($"ctx->gpr[{gpr}]", $"cached_r{gpr}", StringComparison.Ordinal);
        }

        foreach (var fpr in registers.Fprs)
        {
            code = code.Replace($"ctx->fpr[{fpr}]", $"cached_f{fpr}", StringComparison.Ordinal);
        }

        registers = PruneUnusedRegisterCache(code, registers);
        if (registers.IsEmpty && !cacheCr && !cacheXer && !cacheCtr)
        {
            return code;
        }

        var declarationsBuilder = new StringBuilder(
            BuildLeafRegisterCacheDeclarations(registers, initializers));
        if (cacheCr) declarationsBuilder.AppendLine("    uint32_t cached_cr = ctx->cr;");
        if (cacheXer) declarationsBuilder.AppendLine("    uint32_t cached_xer = ctx->xer;");
        if (cacheCtr) declarationsBuilder.AppendLine("    uint32_t cached_ctr = ctx->ctr;");
        if (cacheCr || cacheXer || cacheCtr) declarationsBuilder.AppendLine();
        var declarations = declarationsBuilder.ToString();
        var returnFlushBuilder = new StringBuilder(BuildLeafRegisterCacheFlush(registers));
        if (writeCr) returnFlushBuilder.AppendLine("    ctx->cr = cached_cr;");
        if (writeXer) returnFlushBuilder.AppendLine("    ctx->xer = cached_xer;");
        if (writeCtr) returnFlushBuilder.AppendLine("    ctx->ctr = cached_ctr;");
        var returnFlush = returnFlushBuilder.ToString();
        var hasFlushes = registers.HasFlushes || writeCr || writeXer || writeCtr;
        var lines = code.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var rewritten = new StringBuilder(code.Length + declarations.Length + (returnFlush.Length * 4));
        var functionDecl = FunctionDefinitionSignature(cxxName);
        var pendingFunctionOpen = false;
        var inFunction = false;
        var braceDepth = 0;

        foreach (var line in lines)
        {
            if (!inFunction && string.Equals(line, functionDecl, StringComparison.Ordinal))
            {
                pendingFunctionOpen = true;
            }

            if (pendingFunctionOpen && string.Equals(line, "{", StringComparison.Ordinal))
            {
                rewritten.AppendLine(line);
                rewritten.Append(declarations);
                pendingFunctionOpen = false;
                inFunction = true;
                braceDepth = 1;
                continue;
            }

            if (inFunction && hasFlushes && line.Trim().Equals("return;", StringComparison.Ordinal))
            {
                rewritten.Append(returnFlush);
            }

            rewritten.AppendLine(line);

            if (inFunction)
            {
                braceDepth += CountChar(line, '{') - CountChar(line, '}');
                if (braceDepth <= 0)
                {
                    inFunction = false;
                }
            }
        }

        return rewritten.ToString();
    }

    private static LeafRegisterCache PruneUnusedRegisterCache(string code, LeafRegisterCache registers)
    {
        var gprs = new SortedSet<int>(registers.Gprs
            .Where(gpr => ContainsIdentifier(code, $"cached_r{gpr}")));
        var fprs = new SortedSet<int>(registers.Fprs
            .Where(fpr => ContainsIdentifier(code, $"cached_f{fpr}")));
        var writtenGprs = new SortedSet<int>(registers.WrittenGprs
            .Where(gprs.Contains));
        var writtenFprs = new SortedSet<int>(registers.WrittenFprs
            .Where(fprs.Contains));

        return new LeafRegisterCache(
            gprs, fprs, writtenGprs, writtenFprs, registers.CacheSpecialRegisters);
    }

    /// <summary>
    /// Whole-identifier match, not substring: a plain Contains would treat cached_r1 as live
    /// because cached_r10 appears in the body, leaving an unassigned slot that flushes
    /// ctx->gpr[1] = cached_r1 = 0, zeroing a live architectural register.
    /// </summary>
    private static bool ContainsIdentifier(string code, string identifier)
    {
        var index = 0;
        while (true)
        {
            index = code.IndexOf(identifier, index, StringComparison.Ordinal);
            if (index < 0) return false;
            var end = index + identifier.Length;
            if (end >= code.Length || !IsIdentifierCharacter(code[end])) return true;
            index = end;
        }
    }

    private static bool IsIdentifierCharacter(char value) =>
        value == '_' || char.IsLetterOrDigit(value);

    private static string BuildLeafRegisterCacheDeclarations(
        LeafRegisterCache registers,
        LeafRegisterCacheInitializers initializers)
    {
        var sb = new StringBuilder();
        foreach (var gpr in registers.Gprs)
        {
            var initializer = initializers.Gprs.Contains(gpr)
                ? $"ctx->gpr[{gpr}]"
                : "0";
            sb.AppendLine($"    uint32_t cached_r{gpr} = {initializer};");
        }

        foreach (var fpr in registers.Fprs)
        {
            var initializer = initializers.Fprs.Contains(fpr)
                ? $"ctx->fpr[{fpr}]"
                : "PPC_FPR{}";
            sb.AppendLine($"    PPC_FPR cached_f{fpr} = {initializer};");
        }

        if (!registers.IsEmpty)
        {
            sb.AppendLine();
        }

        return sb.ToString();
    }

    private static string BuildLeafRegisterCacheFlush(LeafRegisterCache registers)
    {
        var sb = new StringBuilder();
        foreach (var gpr in registers.WrittenGprs)
        {
            sb.AppendLine($"    ctx->gpr[{gpr}] = cached_r{gpr};");
        }

        foreach (var fpr in registers.WrittenFprs)
        {
            sb.AppendLine($"    ctx->fpr[{fpr}] = cached_f{fpr};");
        }

        return sb.ToString();
    }

    private static string NormalizeLivenessRegisterName(string registerName) => GetRegisterBaseName(registerName);

    private static int CountChar(string text, char value)
    {
        var count = 0;
        foreach (var ch in text)
        {
            if (ch == value)
            {
                count++;
            }
        }

        return count;
    }
}
