using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Translator.Core.Analysis;
using Translator.Core.Analysis.BasicBlocks;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Translator.Core.Loading;
using Translator.Core.Representation;
using Translator.Core.Translation;

namespace Translator.Core.CodeGen;

/// <summary>What one <see cref="InferredGuestFunctionAbiProvider.Prewarm(IEnumerable{uint}, int)"/> pass did.
/// Returned rather than printed, since only the CLI knows if a pass is worth logging.</summary>
public sealed record GuestAbiPrewarmReport(
    int AddressCount,
    double PhaseASeconds,
    double PhaseBSeconds,
    int PhaseBComputedCount)
{
    public static GuestAbiPrewarmReport Empty { get; } = new(0, 0d, 0d, 0);
}

public sealed class InferredGuestFunctionAbiProvider : IGuestFunctionAbiProvider
{
    private static readonly string[] AbiFloatArgumentRegisters =
        Enumerable.Range(1, 13).Select(static i => $"f{i}").ToArray();

    private static readonly HashSet<string> AbiFloatArgumentSet =
        new(AbiFloatArgumentRegisters, StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Sentinel stored in <see cref="_resolvedTargets"/> for targets that are not guest
    /// addresses inside the image's used range.
    /// </summary>
    private const long UnresolvedTarget = -1;

    private readonly ProgramImage _image;
    private readonly ICanonicalIrProvider? _canonicalIr;

    /// <summary>Lock-free ABI cache so parallel emission never serializes on ABI queries. A plain
    /// dictionary rather than a lazy is deliberate: mutually recursive queries would deadlock under a
    /// per-key computation lock. Worst case is two threads redundantly computing the same value.</summary>
    private readonly ConcurrentDictionary<uint, GuestFunctionAbi> _cache = new();

    /// <summary>Memoizes the (pure) target-string -&gt; guest-address resolution step.</summary>
    private readonly ConcurrentDictionary<string, long> _resolvedTargets = new(StringComparer.Ordinal);

    /// <summary>Recursion/cycle guard plus the phase-A taint stack. Per-thread so a cycle breaks only
    /// within its own call chain, without one thread's in-flight analysis aborting another's query.</summary>
    private readonly ThreadLocal<ThreadAnalysisState> _threadState =
        new(static () => new ThreadAnalysisState());

    /// <summary>Per-thread analysis state; see <see cref="_threadState"/>.</summary>
    private sealed class ThreadAnalysisState
    {
        /// <summary>Addresses whose analysis is in flight on this thread, innermost last.</summary>
        public readonly HashSet<uint> InProgress = new();

        /// <summary>Taint flag per in-flight frame, innermost last. Non-null only during Prewarm phase A;
        /// every other caller sees null and takes the untracked publish-always path.</summary>
        public List<bool>? FrameTaint;

        /// <summary>Reusable backing list for <see cref="FrameTaint"/>; phase A is never reentrant.</summary>
        public readonly List<bool> FrameTaintBuffer = new();

        /// <summary>Addresses already known tainted, shared by every phase-A worker of the current Prewarm
        /// call and reset each invocation. Without this memoization an unpublishable address gets
        /// re-analyzed, subtree and all, by every caller that reaches it, which compounds combinatorially
        /// over deep call chains.</summary>
        public ConcurrentDictionary<uint, byte>? TaintedAddresses;
    }

    public InferredGuestFunctionAbiProvider(
        ProgramImage image,
        ICanonicalIrProvider? canonicalIr = null)
    {
        _image = image;
        _canonicalIr = canonicalIr;
    }

    public bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi)
    {
        if (!TryResolveTargetAddress(target, out var address))
        {
            abi = GuestFunctionAbi.Empty;
            return false;
        }

        if (_cache.TryGetValue(address, out abi!))
        {
            return true;
        }

        var state = _threadState.Value!;
        var inProgress = state.InProgress;
        var frameTaint = state.FrameTaint;

        if (frameTaint is not null && state.TaintedAddresses!.ContainsKey(address))
        {
            // Phase A only: this address is already known unpublishable, so skip re-analyzing its whole
            // uncached subtree and just taint the consumers, matching what would happen anyway.
            TaintFramesInFlight(frameTaint);
            abi = GuestFunctionAbi.Empty;
            return false;
        }

        if (!inProgress.Add(address))
        {
            // Cycle break: the only schedule-dependent event in this analysis. The returned value depends
            // on which entry point started the call chain, so every frame in flight here is tainted.
            if (frameTaint is not null)
            {
                TaintFramesInFlight(frameTaint);
                state.TaintedAddresses!.TryAdd(address, 0);
            }

            abi = GuestFunctionAbi.Empty;
            return false;
        }

        if (frameTaint is null)
        {
            // Untracked path: normal emission-time queries and Prewarm phase B. Publishes
            // unconditionally, exactly as this method always has.
            try
            {
                abi = Analyze(address);
                _cache[address] = abi;
                return true;
            }
            catch
            {
                abi = GuestFunctionAbi.Empty;
                _cache[address] = abi;
                return false;
            }
            finally
            {
                inProgress.Remove(address);
            }
        }

        // Prewarm phase A: track taint for this frame and publish only if it stayed clean.
        frameTaint.Add(false);
        var frameIndex = frameTaint.Count - 1;
        bool succeeded;
        bool tainted;
        try
        {
            try
            {
                abi = Analyze(address);
                succeeded = true;
            }
            catch
            {
                // Whether Analyze throws can itself depend on values this frame consumed, so the fallback
                // follows the same taint rule as a success: a tainted frame publishes nothing.
                abi = GuestFunctionAbi.Empty;
                succeeded = false;
            }
        }
        finally
        {
            inProgress.Remove(address);
            tainted = frameTaint[frameIndex];
            frameTaint.RemoveAt(frameIndex);
            if (tainted && frameIndex > 0)
            {
                // The caller consumed a locally computed, unpublished (schedule-dependent) value.
                frameTaint[frameIndex - 1] = true;
            }
        }

        if (tainted)
        {
            // Remember that this address cannot be published so no other frame - on this thread or any
            // other - ever pays for analyzing it (or its subtree) again during this Prewarm.
            state.TaintedAddresses!.TryAdd(address, 0);
        }
        else
        {
            _cache[address] = abi;
        }

        return succeeded;
    }

    /// <summary>Marks every frame in flight on this thread as tainted: it consumed a
    /// schedule-dependent value and is therefore unpublishable.</summary>
    private static void TaintFramesInFlight(List<bool> frameTaint)
    {
        for (var index = 0; index < frameTaint.Count; index++)
        {
            frameTaint[index] = true;
        }
    }

    /// <summary>Populates the cache so a later parallel emission wave sees pure cache hits and mutually
    /// recursive ABI cycles resolve deterministically. Byte-identical to a sequential ascending prewarm;
    /// see the two-parameter overload for why.</summary>
    public GuestAbiPrewarmReport Prewarm(IEnumerable<uint> addresses) =>
        Prewarm(addresses, degreeOfParallelism: 0);

    /// <summary>
    /// Two-phase prewarm, provably identical to the old sequential ascending pass (~38.7s for the full
    /// image). Phase A analyzes addresses in parallel with taint tracking: a frame taints if a cycle
    /// break happens while it's in flight or it consumes a tainted result, and only untainted
    /// (schedule-independent) frames publish to the cache, so parallel order can never change a cached
    /// value. A shared already-tainted set stops combinatorial blowup on deep call chains (measured
    /// 12,500+ CPU-seconds without it, vs. ~39s sequential). Phase B then walks the same list ascending,
    /// filling in whatever phase A left uncached exactly like the old loop, which reproduces identical
    /// SCC cycle-break results since ascending order is what picks each SCC's entry point.
    /// </summary>
    /// <param name="addresses">Addresses to analyze; de-duplicated and sorted internally.</param>
    /// <param name="degreeOfParallelism">
    /// Phase A worker count. Zero or negative selects <see cref="Environment.ProcessorCount"/>.
    /// </param>
    public GuestAbiPrewarmReport Prewarm(IEnumerable<uint> addresses, int degreeOfParallelism)
    {
        var ordered = addresses.Distinct().OrderBy(static address => address).ToArray();
        if (ordered.Length == 0)
        {
            return GuestAbiPrewarmReport.Empty;
        }

        var workers = degreeOfParallelism > 0 ? degreeOfParallelism : Environment.ProcessorCount;
        var phaseClock = System.Diagnostics.Stopwatch.StartNew();
        if (workers > 1 && ordered.Length > 1)
        {
            // Fresh per invocation: taint is only meaningful relative to the cache state this pass
            // started from, and phase B may well have cached some of it by the next call.
            var taintedAddresses = new ConcurrentDictionary<uint, byte>();
            var options = new ParallelOptions { MaxDegreeOfParallelism = workers };
            Parallel.ForEach(ordered, options, address => PrewarmTracked(address, taintedAddresses));
        }
        var phaseASeconds = phaseClock.Elapsed.TotalSeconds;

        var phaseBComputed = 0;
        foreach (var address in ordered)
        {
            if (_cache.ContainsKey(address))
            {
                continue;
            }

            phaseBComputed++;
            TryGetGuestFunctionAbi(FunctionTargetName(address), out _);
        }
        return new GuestAbiPrewarmReport(
            ordered.Length,
            phaseASeconds,
            phaseClock.Elapsed.TotalSeconds - phaseASeconds,
            phaseBComputed);
    }

    /// <summary>Phase A body: analyze one address with taint tracking enabled for this thread.</summary>
    private void PrewarmTracked(uint address, ConcurrentDictionary<uint, byte> taintedAddresses)
    {
        if (_cache.ContainsKey(address) || taintedAddresses.ContainsKey(address))
        {
            return;
        }

        var state = _threadState.Value!;
        var frames = state.FrameTaintBuffer;
        frames.Clear();
        state.TaintedAddresses = taintedAddresses;
        state.FrameTaint = frames;
        try
        {
            TryGetGuestFunctionAbi(FunctionTargetName(address), out _);
        }
        catch
        {
            // Phase A is pure speculation: anything that escapes here leaves the address uncached, and
            // phase B reproduces the original sequential behaviour (including rethrowing) for it. Never
            // let it tear down the parallel loop, which the sequential prewarm could not have done.
        }
        finally
        {
            state.FrameTaint = null;
            state.TaintedAddresses = null;
        }
    }

    private static string FunctionTargetName(uint address) => $"func_{address:X8}";

    private bool TryResolveTargetAddress(string target, out uint address)
    {
        if (_resolvedTargets.TryGetValue(target, out var cached))
        {
            address = cached == UnresolvedTarget ? 0u : (uint)cached;
            return cached != UnresolvedTarget;
        }

        var resolved = UnresolvedTarget;
        if (GuestTargetParser.TryParseAddress(target, out var parsed) && _image.UsedRange.Contains(parsed))
        {
            resolved = parsed;
        }

        _resolvedTargets[target] = resolved;
        address = resolved == UnresolvedTarget ? 0u : (uint)resolved;
        return resolved != UnresolvedTarget;
    }

    private GuestFunctionAbi Analyze(uint entryPoint)
    {
        IrFunction? canonical = null;
        var hasCanonical = _canonicalIr?.TryGet(entryPoint, out canonical!) == true;
        var linear = hasCanonical ? canonical! : BuildLinearIr(entryPoint);

        // IrCfg.Build is pure; build the linear CFG once and share it across every consumer that
        // used to rebuild it (argument inference, scalar-float inference and the canonical SsaResult).
        var linearCfg = IrCfg.Build(linear);
        var argumentRegisters = InferArgumentRegisters(linear, linearCfg);
        var scalarFloatArgs = InferScalarFloatArguments(linear, linearCfg);
        var returnsPairedScalarFloat = false;
        var writesFloatReturnRegister = false;

        try
        {
            var ssa = hasCanonical
                ? new SsaResult(linear, linearCfg)
                : new SsaTransformer().Convert(linear);
            ssa.ValidateUseDef();
            var types = new RepresentationClassifier().Classify(ssa.Function);
            var signature = new FunctionAbiClassifier().Classify($"func_{entryPoint:X8}", ssa.Function, types);
            writesFloatReturnRegister = signature.ReturnRepresentation is ValueRepresentation { IsFloat: true };

            if (writesFloatReturnRegister)
            {
                // The SSA transformer returns a *renamed* function with phi nodes inserted, so its
                // CFG is not necessarily the linear one; only reuse linearCfg for the same function.
                // Kept behind the float-return check so the CFG is built exactly when it was before.
                var ssaCfg = ReferenceEquals(ssa.Function, linear) ? linearCfg : IrCfg.Build(ssa.Function);
                returnsPairedScalarFloat = InferReturnsPairedScalarFloat(ssa.Function, ssaCfg, signature);
            }
        }
        catch
        {
            returnsPairedScalarFloat = false;
            writesFloatReturnRegister = false;
        }

        return new GuestFunctionAbi(
            scalarFloatArgs,
            returnsPairedScalarFloat,
            writesFloatReturnRegister,
            argumentRegisters: argumentRegisters);
    }

    private IrFunction BuildLinearIr(uint entryPoint)
    {
        using var disassembler = new PpcDisassembler();
        var instructions = disassembler.DisassembleFunction(_image, entryPoint, maxInstructions: 8192, maxBytes: 0x10000);
        var basicBlocks = BasicBlockBuilder.Build(instructions);
        var lifted = new PpcLifter().Lift(instructions, true);
        var liftedByAddress = lifted.ToDictionary(x => x.Origin.Address, x => x.Ir);

        var irBlocks = new List<IrBasicBlock>(basicBlocks.Count);
        foreach (var block in basicBlocks)
        {
            var ir = new List<IrInstruction>();
            foreach (var ins in block.Instructions)
            {
                if (!liftedByAddress.TryGetValue(ins.Address, out var lowered))
                {
                    throw new InvalidOperationException($"Failed to lift instruction at 0x{ins.Address:X8}");
                }

                ir.AddRange(lowered);
            }

            irBlocks.Add(new IrBasicBlock(LabelFor(block.StartAddress), ir));
        }

        AddMissingReferencedBlocks(irBlocks);
        return new IrFunction($"func_{entryPoint:X8}", irBlocks.FirstOrDefault()?.Label ?? "entry", irBlocks);
    }

    private static void AddMissingReferencedBlocks(List<IrBasicBlock> irBlocks)
    {
        var existingLabels = new HashSet<string>(irBlocks.Select(static b => b.Label), StringComparer.OrdinalIgnoreCase);
        var referencedLabels = irBlocks
            .SelectMany(static b => b.Instructions.OfType<IrBranch>().SelectMany(static br => new[] { br.TrueLabel, br.FalseLabel }))
            .Concat(irBlocks.SelectMany(static b => b.Instructions.OfType<IrJump>().Select(static j => j.TargetLabel)))
            .Concat(irBlocks.SelectMany(static b => b.Instructions.OfType<IrJumpTable>().SelectMany(static jt => jt.Cases.Select(static c => c.TargetLabel))))
            .Where(static l => !string.IsNullOrWhiteSpace(l))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (var label in referencedLabels)
        {
            if (existingLabels.Contains(label))
            {
                continue;
            }

            IrInstruction instruction = string.Equals(label, "return", StringComparison.OrdinalIgnoreCase)
                ? new IrReturn(null)
                : new IrComment("synthetic missing target");
            irBlocks.Add(new IrBasicBlock(label, new IrInstruction[] { instruction }));
            existingLabels.Add(label);
        }
    }

    private HashSet<string> InferArgumentRegisters(IrFunction function, IrCfg cfg)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var blockUses = new Dictionary<string, HashSet<string>>(comparer);
        var blockDefs = new Dictionary<string, HashSet<string>>(comparer);

        foreach (var block in function.Blocks)
        {
            var uses = new HashSet<string>(comparer);
            var defs = new HashSet<string>(comparer);
            foreach (var instruction in block.Instructions)
            {
                foreach (var use in InstructionRegisterUsesForArgumentInference(instruction))
                {
                    if (!defs.Contains(use))
                    {
                        uses.Add(use);
                    }
                }

                foreach (var def in InstructionRegisterDefsForArgumentInference(instruction))
                {
                    defs.Add(def);
                }
            }

            blockUses[block.Label] = uses;
            blockDefs[block.Label] = defs;
        }

        var liveIn = function.Blocks.ToDictionary(
            static block => block.Label,
            _ => new HashSet<string>(comparer),
            comparer);
        var liveOut = function.Blocks.ToDictionary(
            static block => block.Label,
            _ => new HashSet<string>(comparer),
            comparer);

        var changed = true;
        while (changed)
        {
            changed = false;
            for (var blockIndex = function.Blocks.Count - 1; blockIndex >= 0; blockIndex--)
            {
                var block = function.Blocks[blockIndex];
                var newOut = new HashSet<string>(comparer);
                foreach (var successor in cfg.Successors(block.Label))
                {
                    if (liveIn.TryGetValue(successor, out var successorLiveIn))
                    {
                        newOut.UnionWith(successorLiveIn);
                    }
                }

                var newIn = new HashSet<string>(blockUses[block.Label], comparer);
                var outMinusDefs = new HashSet<string>(newOut, comparer);
                outMinusDefs.ExceptWith(blockDefs[block.Label]);
                newIn.UnionWith(outMinusDefs);

                if (!newOut.SetEquals(liveOut[block.Label]))
                {
                    liveOut[block.Label] = newOut;
                    changed = true;
                }

                if (!newIn.SetEquals(liveIn[block.Label]))
                {
                    liveIn[block.Label] = newIn;
                    changed = true;
                }
            }
        }

        return liveIn.TryGetValue(function.EntryLabel, out var entryLive)
            ? entryLive
                .Where(IsGprOrFprRegister)
                .ToHashSet(comparer)
            : new HashSet<string>(comparer);
    }

    private IEnumerable<string> InstructionRegisterUsesForArgumentInference(IrInstruction instruction)
    {
        static IEnumerable<IrValue> Values(params IrValue[] values) => values;

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
                if (!string.IsNullOrWhiteSpace(load.Address.Base))
                {
                    yield return Base(load.Address.Base);
                }
                break;

            case IrStore store:
                if (!string.IsNullOrWhiteSpace(store.Address.Base))
                {
                    yield return Base(store.Address.Base);
                }
                foreach (var use in RegisterUsesFromValues(Values(store.Source)))
                {
                    yield return use;
                }
                break;

            case IrCall call:
                if (IsInlineSaveThunk(call))
                {
                    break;
                }

                foreach (var use in RegisterUsesFromValues(CallArgumentsForArgumentInference(call)))
                {
                    yield return use;
                }
                break;

            case IrIndirectCall indirectCall:
                foreach (var use in RegisterUsesFromValues(Values(indirectCall.Target)))
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
                    yield return Base(source);
                }
                break;

            case IrReturn ret:
                if (ret.Value is { } value)
                {
                    foreach (var use in RegisterUsesFromValues(Values(value)))
                    {
                        yield return use;
                    }
                }
                break;

            case IrBranch branch:
                if (IsGuestRegisterName(branch.ConditionRegister))
                {
                    yield return Base(branch.ConditionRegister);
                }
                break;

            case IrJumpTable table:
                if (!string.IsNullOrWhiteSpace(table.Selector))
                {
                    yield return Base(table.Selector);
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

    private static IEnumerable<string> InstructionRegisterDefsForArgumentInference(IrInstruction instruction)
    {
        switch (instruction)
        {
            case IrAssign assign:
                yield return Base(assign.Destination);
                break;

            case IrBinary binary:
                yield return Base(binary.Destination);
                break;

            case IrLoad load:
                yield return Base(load.Destination);
                break;

            case IrCall call:
                if (!string.IsNullOrWhiteSpace(call.Destination))
                {
                    yield return Base(call.Destination);
                }
                foreach (var def in InlineThunkDefs(call))
                {
                    yield return def;
                }
                if (IsGuestCallTarget(call.Target))
                {
                    yield return "r3";
                    yield return "r4";
                    yield return "f1";
                }
                break;

            case IrIndirectCall indirectCall:
                if (!string.IsNullOrWhiteSpace(indirectCall.Destination))
                {
                    yield return Base(indirectCall.Destination);
                }
                yield return "r3";
                yield return "r4";
                yield return "f1";
                break;

            case IrPhi phi:
                yield return Base(phi.Destination);
                break;
        }
    }

    private IReadOnlyList<IrValue> CallArgumentsForArgumentInference(IrCall call)
    {
        if (!IsGuestCallTarget(call.Target) ||
            !TryGetGuestFunctionAbi(call.Target, out var abi) ||
            !abi.HasKnownArgumentRegisters)
        {
            return call.Arguments;
        }

        return call.Arguments
            .Where(arg =>
                arg.Kind != "register" ||
                arg.RegisterName is null ||
                abi.HasArgumentRegister(arg.RegisterName))
            .ToArray();
    }

    private static IEnumerable<string> RegisterUsesFromValues(IEnumerable<IrValue> values)
    {
        foreach (var value in values)
        {
            if (value.Kind == "register" && !string.IsNullOrWhiteSpace(value.RegisterName))
            {
                yield return Base(value.RegisterName);
            }
        }
    }

    // The three thunk-aware passes share one set of ranges from the project's function map. This site is
    // the odd one out: it feeds (FirstRegister, LastRegister) into params IsInlineThunkRange reads as
    // (index bound, register base), so it accepts a shorter run and reports different start registers
    // than the other two callers. Preserved verbatim since every shipped translation used it, but it's
    // almost certainly a latent bug, not a design.
    private static bool IsInlineSaveThunk(IrCall call) =>
        GuestTargetParser.TryParseAddress(call.Target, out var address) &&
        (IsInlineThunkRange(address, GuestSaveRestoreThunks.Current.SaveGpr) ||
         IsInlineThunkRange(address, GuestSaveRestoreThunks.Current.SaveFpr));

    private static IEnumerable<string> InlineThunkDefs(IrCall call)
    {
        if (!GuestTargetParser.TryParseAddress(call.Target, out var address))
        {
            yield break;
        }

        if (IsInlineThunkRange(address, GuestSaveRestoreThunks.Current.RestGpr, out var gprStart))
        {
            for (var reg = gprStart; reg <= 31; reg++)
            {
                yield return $"r{reg}";
            }
            yield break;
        }

        if (IsInlineThunkRange(address, GuestSaveRestoreThunks.Current.RestFpr, out var fprStart))
        {
            for (var reg = fprStart; reg <= 31; reg++)
            {
                yield return $"f{reg}";
            }
        }
    }

    private static bool IsInlineThunkRange(uint address, GuestSaveRestoreThunkRange? range) =>
        IsInlineThunkRange(address, range, out _);

    private static bool IsInlineThunkRange(
        uint address,
        GuestSaveRestoreThunkRange? range,
        out int startRegister)
    {
        startRegister = 0;
        if (range is null)
        {
            return false;
        }

        var (baseAddress, count, baseRegister) = range;
        if (address < baseAddress)
        {
            return false;
        }

        var delta = address - baseAddress;
        if ((delta % 4) != 0)
        {
            return false;
        }

        var index = (int)(delta / 4);
        if (index < 0 || index > count)
        {
            return false;
        }

        startRegister = baseRegister + index;
        return startRegister <= 31;
    }

    private HashSet<string> InferScalarFloatArguments(IrFunction function, IrCfg cfg)
    {
        var scalarArgs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var comparer = StringComparer.OrdinalIgnoreCase;
        var inMap = function.Blocks.ToDictionary(
            static b => b.Label,
            b => b.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase)
                ? CreateInitialArgumentAliasState()
                : new Dictionary<string, HashSet<string>>(comparer),
            comparer);
        var outMap = function.Blocks.ToDictionary(
            static b => b.Label,
            b => b.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase)
                ? CreateInitialArgumentAliasState()
                : new Dictionary<string, HashSet<string>>(comparer),
            comparer);

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var block in function.Blocks)
            {
                var newIn = block.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase)
                    ? CreateInitialArgumentAliasState()
                    : UnionPredecessorStates(cfg.Predecessors(block.Label), outMap);

                var newOut = CopyAliasState(newIn);
                foreach (var ins in block.Instructions)
                {
                    ProcessScalarArgumentInstruction(ins, newOut, scalarArgs);
                }

                if (!AliasStatesEqual(newIn, inMap[block.Label]))
                {
                    inMap[block.Label] = newIn;
                    changed = true;
                }

                if (!AliasStatesEqual(newOut, outMap[block.Label]))
                {
                    outMap[block.Label] = newOut;
                    changed = true;
                }
            }
        }

        return scalarArgs;
    }

    private void ProcessScalarArgumentInstruction(
        IrInstruction ins,
        Dictionary<string, HashSet<string>> aliases,
        HashSet<string> scalarArgs)
    {
        switch (ins)
        {
            case IrAssign assign:
                PropagateFloatAlias(assign.Destination, assign.Value, aliases);
                break;

            case IrBinary bin:
                MarkScalarUse(bin.Left, aliases, scalarArgs);
                MarkScalarUse(bin.Right, aliases, scalarArgs);
                KillFloatAlias(bin.Destination, aliases);
                break;

            case IrLoad load:
                KillFloatAlias(load.Destination, aliases);
                break;

            case IrStore store:
                if (store.SizeBytes is 4 or 8)
                {
                    MarkScalarUse(store.Source, aliases, scalarArgs);
                }
                break;

            case IrCall call:
                ProcessScalarArgumentCall(call.Target, call.Arguments, aliases, scalarArgs);
                KillFloatAlias(call.Destination, aliases);
                break;

            case IrIndirectCall icall:
                foreach (var arg in icall.Arguments)
                {
                    MarkScalarUse(arg, aliases, scalarArgs);
                }
                aliases.Remove("f1");
                KillFloatAlias(icall.Destination, aliases);
                break;

            case IrSetCrField setCr:
                MarkScalarUse(setCr.Left, aliases, scalarArgs);
                MarkScalarUse(setCr.Right, aliases, scalarArgs);
                break;

            case IrPhi phi:
                KillFloatAlias(phi.Destination, aliases);
                break;
        }
    }

    private void ProcessScalarArgumentCall(
        string target,
        IReadOnlyList<IrValue> arguments,
        Dictionary<string, HashSet<string>> aliases,
        HashSet<string> scalarArgs)
    {
        if (IsGuestCallTarget(target))
        {
            var writesFloatReturnRegister = true;
            if (TryGetGuestFunctionAbi(target, out var abi))
            {
                writesFloatReturnRegister = abi.WritesFloatReturnRegister;
                foreach (var arg in arguments)
                {
                    if (arg.Kind == "register" &&
                        arg.RegisterName != null &&
                        abi.HasScalarFloatArgument(Base(arg.RegisterName)))
                    {
                        MarkScalarUse(arg, aliases, scalarArgs);
                    }
                }
            }

            if (writesFloatReturnRegister)
            {
                aliases.Remove("f1");
            }
            return;
        }

        if (!IsScalarHelperConsumer(target))
        {
            return;
        }

        foreach (var arg in arguments)
        {
            MarkScalarUse(arg, aliases, scalarArgs);
        }
    }

    private static bool IsScalarHelperConsumer(string target)
    {
        if (target.Equals("PPC_Fres", StringComparison.OrdinalIgnoreCase) ||
            target.Equals("PPC_Frsqrte", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return !PpcFloatCallSemantics.IsPairedConsumerTarget(target) &&
               PpcFloatCallSemantics.IsScalarFloatConsumerTarget(target);
    }

    private static bool InferReturnsPairedScalarFloat(
        IrFunction function,
        IrCfg cfg,
        FunctionAbiClassification signature)
    {
        if (signature.ReturnRepresentation is not ValueRepresentation { IsFloat: true })
        {
            return false;
        }

        var allFloatRegisters = Enumerable.Range(0, 32)
            .Select(static i => $"f{i}")
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        var inMap = function.Blocks.ToDictionary(
            static b => b.Label,
            b => b.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase)
                ? new HashSet<string>(StringComparer.OrdinalIgnoreCase)
                : new HashSet<string>(allFloatRegisters, StringComparer.OrdinalIgnoreCase),
            StringComparer.OrdinalIgnoreCase);
        var outMap = function.Blocks.ToDictionary(
            static b => b.Label,
            b => b.Label.Equals(function.EntryLabel, StringComparison.OrdinalIgnoreCase)
                ? new HashSet<string>(StringComparer.OrdinalIgnoreCase)
                : new HashSet<string>(allFloatRegisters, StringComparer.OrdinalIgnoreCase),
            StringComparer.OrdinalIgnoreCase);

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var block in function.Blocks)
            {
                var preds = cfg.Predecessors(block.Label).Where(p => !p.Equals(block.Label, StringComparison.OrdinalIgnoreCase)).ToList();
                HashSet<string> newIn;
                if (preds.Count == 0)
                {
                    newIn = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                }
                else
                {
                    newIn = new HashSet<string>(outMap[preds[0]], StringComparer.OrdinalIgnoreCase);
                    foreach (var pred in preds.Skip(1))
                    {
                        newIn.IntersectWith(outMap[pred]);
                    }
                }

                var newOut = new HashSet<string>(newIn, StringComparer.OrdinalIgnoreCase);
                foreach (var ins in block.Instructions)
                {
                    ProcessReturnPairedInstruction(ins, outMap, newOut);
                }

                if (!newIn.SetEquals(inMap[block.Label]))
                {
                    inMap[block.Label] = newIn;
                    changed = true;
                }

                if (!newOut.SetEquals(outMap[block.Label]))
                {
                    outMap[block.Label] = newOut;
                    changed = true;
                }
            }
        }

        var returnBlocks = function.Blocks
            .Where(static b => b.Instructions.LastOrDefault() is IrReturn)
            .ToList();
        return returnBlocks.Count > 0 &&
               returnBlocks.All(b => outMap.TryGetValue(b.Label, out var state) && state.Contains("f1"));
    }

    private static void ProcessReturnPairedInstruction(
        IrInstruction ins,
        IReadOnlyDictionary<string, HashSet<string>> blockOut,
        HashSet<string> pairedSet)
    {
        switch (ins)
        {
            case IrPhi phi:
                {
                    var destBase = Base(phi.Destination);
                    if (!IsFloatRegister(destBase))
                    {
                        break;
                    }

                    var allPaired = true;
                    foreach (var (predLabel, src) in phi.Sources)
                    {
                        var srcBase = Base(src);
                        if (!IsFloatRegister(srcBase) ||
                            !blockOut.TryGetValue(predLabel, out var predOut) ||
                            !predOut.Contains(srcBase))
                        {
                            allPaired = false;
                            break;
                        }
                    }

                    if (allPaired)
                    {
                        pairedSet.Add(destBase);
                    }
                    else
                    {
                        pairedSet.Remove(destBase);
                    }
                    break;
                }

            case IrCall call:
                if (IsGuestCallTarget(call.Target))
                {
                    pairedSet.Remove("f1");
                }

                if (!string.IsNullOrWhiteSpace(call.Destination))
                {
                    var destBase = Base(call.Destination);
                    if (IsFloatRegister(destBase))
                    {
                        if (PpcFloatCallSemantics.IsPairedProducerTarget(call.Target))
                        {
                            pairedSet.Add(destBase);
                        }
                        else
                        {
                            pairedSet.Remove(destBase);
                        }
                    }
                }
                break;

            case IrIndirectCall icall:
                pairedSet.Remove("f1");
                KillFloatPairedState(icall.Destination, pairedSet);
                break;

            case IrAssign assign:
                {
                    var destBase = Base(assign.Destination);
                    if (!IsFloatRegister(destBase))
                    {
                        break;
                    }

                    if (assign.Value.Kind == "register" &&
                        assign.Value.RegisterName != null &&
                        IsFloatRegister(Base(assign.Value.RegisterName)) &&
                        pairedSet.Contains(Base(assign.Value.RegisterName)))
                    {
                        pairedSet.Add(destBase);
                    }
                    else
                    {
                        pairedSet.Remove(destBase);
                    }
                    break;
                }

            case IrBinary bin:
                KillFloatPairedState(bin.Destination, pairedSet);
                break;

            case IrLoad load:
                KillFloatPairedState(load.Destination, pairedSet);
                break;
        }
    }

    private static void MarkScalarUse(
        IrValue value,
        IReadOnlyDictionary<string, HashSet<string>> aliases,
        HashSet<string> scalarArgs)
    {
        if (value.Kind != "register" || value.RegisterName == null)
        {
            return;
        }

        var baseName = Base(value.RegisterName);
        if (!IsFloatRegister(baseName) || !aliases.TryGetValue(baseName, out var sources))
        {
            return;
        }

        foreach (var source in sources)
        {
            if (AbiFloatArgumentSet.Contains(source))
            {
                scalarArgs.Add(source);
            }
        }
    }

    private static void PropagateFloatAlias(
        string destination,
        IrValue value,
        Dictionary<string, HashSet<string>> aliases)
    {
        var destBase = Base(destination);
        if (!IsFloatRegister(destBase))
        {
            return;
        }

        if (value.Kind == "register" &&
            value.RegisterName != null &&
            IsFloatRegister(Base(value.RegisterName)) &&
            aliases.TryGetValue(Base(value.RegisterName), out var srcAliases))
        {
            aliases[destBase] = new HashSet<string>(srcAliases, StringComparer.OrdinalIgnoreCase);
        }
        else
        {
            aliases.Remove(destBase);
        }
    }

    private static void KillFloatAlias(string destination, Dictionary<string, HashSet<string>> aliases)
    {
        if (!string.IsNullOrWhiteSpace(destination) && IsFloatRegister(Base(destination)))
        {
            aliases.Remove(Base(destination));
        }
    }

    private static void KillFloatPairedState(string destination, HashSet<string> pairedSet)
    {
        if (!string.IsNullOrWhiteSpace(destination) && IsFloatRegister(Base(destination)))
        {
            pairedSet.Remove(Base(destination));
        }
    }

    private static Dictionary<string, HashSet<string>> CreateInitialArgumentAliasState()
    {
        return AbiFloatArgumentRegisters.ToDictionary(
            static r => r,
            static r => new HashSet<string>(StringComparer.OrdinalIgnoreCase) { r },
            StringComparer.OrdinalIgnoreCase);
    }

    private static Dictionary<string, HashSet<string>> UnionPredecessorStates(
        IReadOnlyList<string> predecessors,
        IReadOnlyDictionary<string, Dictionary<string, HashSet<string>>> outMap)
    {
        var result = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var pred in predecessors)
        {
            if (!outMap.TryGetValue(pred, out var predState))
            {
                continue;
            }

            foreach (var (register, sources) in predState)
            {
                if (!result.TryGetValue(register, out var destSources))
                {
                    destSources = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    result[register] = destSources;
                }

                destSources.UnionWith(sources);
            }
        }

        return result;
    }

    private static Dictionary<string, HashSet<string>> CopyAliasState(
        IReadOnlyDictionary<string, HashSet<string>> state)
    {
        return state.ToDictionary(
            static kv => kv.Key,
            static kv => new HashSet<string>(kv.Value, StringComparer.OrdinalIgnoreCase),
            StringComparer.OrdinalIgnoreCase);
    }

    private static bool AliasStatesEqual(
        IReadOnlyDictionary<string, HashSet<string>> left,
        IReadOnlyDictionary<string, HashSet<string>> right)
    {
        if (left.Count != right.Count)
        {
            return false;
        }

        foreach (var (key, leftSet) in left)
        {
            if (!right.TryGetValue(key, out var rightSet) || !leftSet.SetEquals(rightSet))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsGuestCallTarget(string target)
    {
        if (string.IsNullOrWhiteSpace(target))
        {
            return false;
        }

        if (GuestTargetParser.TryParseAddress(target, out _))
        {
            return true;
        }

        var trimmed = target.Trim();
        return trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ||
               trimmed.StartsWith("func_", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsFloatRegister(string name)
    {
        var baseName = Base(name);
        if (baseName.Length < 2 || baseName.Length > 3) return false;
        if (baseName[0] != 'f') return false;
        if (!char.IsDigit(baseName[1])) return false;
        if (baseName.Length == 2) return true;
        return char.IsDigit(baseName[2]) && int.Parse(baseName[1..], CultureInfo.InvariantCulture) < 32;
    }

    private static bool IsGprOrFprRegister(string name)
    {
        var baseName = Base(name);
        if (baseName.Length < 2 || baseName.Length > 3)
        {
            return false;
        }

        if (baseName[0] is not ('r' or 'f') || !char.IsDigit(baseName[1]))
        {
            return false;
        }

        if (!int.TryParse(baseName[1..], NumberStyles.None, CultureInfo.InvariantCulture, out var index))
        {
            return false;
        }

        return index is >= 0 and < 32;
    }

    private static string Base(string name) => RegisterNameUtils.StripNumericSuffix(name);

    private static bool IsGuestRegisterName(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return false;
        }

        var baseName = Base(name);
        if (IsGprOrFprRegister(baseName))
        {
            return true;
        }

        if (baseName.Length == 3 &&
            baseName.StartsWith("cr", StringComparison.OrdinalIgnoreCase) &&
            char.IsDigit(baseName[2]) &&
            (baseName[2] - '0') is >= 0 and <= 7)
        {
            return true;
        }

        return baseName.Equals("cr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("lr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("xer", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("fpscr", StringComparison.OrdinalIgnoreCase);
    }

    private static string LabelFor(uint address) => $"0x{address:X8}";

}
