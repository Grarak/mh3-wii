using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// One save or restore of the caller's r14-r31 values: an instruction at
/// <see cref="Index"/> of block <see cref="Label"/> that moves only the
/// registers in <see cref="Registers"/> between their locals and their save slots.
/// </summary>
public readonly record struct NonvolatileSaveRestore(string Label, int Index, uint Registers);

public sealed record NonvolatileGprPreservation(
    uint PreservedMask,
    IReadOnlyList<NonvolatileSaveRestore> SaveRestores,
    string? Reason = null)
{
    public static NonvolatileGprPreservation None { get; } = new(0, Array.Empty<NonvolatileSaveRestore>());
}

/// <summary>
/// Which of r14-r31 a function preserves the way the PowerPC EABI promises: the value it was
/// called with is only ever stored to a stack save slot, and every exit either never changed the
/// register or reloads it from that slot. Callers keep such a register in a host local across the
/// call, so it never needs to reach CpuContext, and the function can drop its save/restore pair.
/// Callees are consulted through their contracts: a callee that reads or changes rN without
/// restoring it makes rN unpreserved here too, unless this function restores rN itself afterwards.
/// </summary>
public static class NonvolatileGprPreservationAnalyzer
{
    public const uint NonvolatileGprMask = 0xFFFFC000u;

    internal readonly record struct Slot(string Root, long Offset);

    public static NonvolatileGprPreservation Analyze(
        IrFunction function,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts)
    {
        if (function.Blocks.Count == 0) return NonvolatileGprPreservation.None;
        var symbols = StackSymbols.Build(function);
        if (symbols is null)
        {
            if (DebugFunction is not null && function.Name.Contains(DebugFunction, StringComparison.OrdinalIgnoreCase))
                Console.WriteLine($"[nv] {function.Name}: not in SSA form");
            return NonvolatileGprPreservation.None;
        }
        var cfg = IrCfg.Build(function);
        var saveSlots = new Dictionary<Slot, uint>();
        for (var round = 0; round < 6; ++round)
        {
            var inStates = Solve(function, cfg, symbols, saveSlots, calleeContracts);
            var walk = new Recorder(saveSlots)
            {
                Debug = DebugFunction is not null &&
                        function.Name.Contains(DebugFunction, StringComparison.OrdinalIgnoreCase)
            };
            Check(function, cfg, symbols, calleeContracts, inStates, walk);
            if (walk.Debug)
                Console.WriteLine($"[nv] round {round}: slots {string.Join(", ", walk.NewSaveSlots.Select(p => $"{p.Key.Root}{p.Key.Offset:+#;-#;+0}=0x{p.Value:X8}"))}");
            if (SameSlots(walk.NewSaveSlots, saveSlots))
            {
                var preserved = NonvolatileGprMask & ~walk.Bad;
                var saveRestores = walk.SaveRestores
                    .Where(item => (item.Registers & ~preserved) == 0)
                    .ToArray();
                return new NonvolatileGprPreservation(preserved, saveRestores, walk.Reason);
            }
            saveSlots = walk.NewSaveSlots;
        }
        return NonvolatileGprPreservation.None;
    }

    private static bool SameSlots(Dictionary<Slot, uint> left, Dictionary<Slot, uint> right) =>
        left.Count == right.Count &&
        left.All(pair => right.TryGetValue(pair.Key, out var mask) && mask == pair.Value);

    private sealed class Recorder
    {
        public Recorder(Dictionary<Slot, uint> saveSlots) => SaveSlots = saveSlots;
        public Dictionary<Slot, uint> SaveSlots { get; }
        public Dictionary<Slot, uint> NewSaveSlots { get; } = new();
        public uint Bad;
        public bool Debug;
        /// <summary>The first instruction that cost a register, for diagnostics.</summary>
        public string? Reason;
        public List<NonvolatileSaveRestore> SaveRestores { get; } = new();
    }

    /// <summary>Walk state threaded through <see cref="Step"/>.</summary>
    private struct Walk
    {
        public uint State;
        public bool Dead;
        /// <summary>SSA name r11 last took in this block, for the save/restore thunks' frame base.</summary>
        public string? R11;
        public uint Bad;
        public uint SaveRestoreRegisters;
        public bool Impure;
    }

    private static Dictionary<string, uint> Solve(
        IrFunction function,
        IrCfg cfg,
        StackSymbols symbols,
        Dictionary<Slot, uint> saveSlots,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var inStates = new Dictionary<string, uint>(comparer) { [cfg.Entry] = NonvolatileGprMask };
        var outStates = new Dictionary<string, uint?>(comparer);
        var worklist = new Queue<string>();
        var queued = new HashSet<string>(comparer);
        worklist.Enqueue(cfg.Entry);
        queued.Add(cfg.Entry);
        while (worklist.Count > 0)
        {
            var label = worklist.Dequeue();
            queued.Remove(label);
            var walk = new Walk { State = inStates[label] };
            var instructions = cfg.Blocks[label].Instructions;
            for (var index = 0; index < instructions.Count && !walk.Dead; ++index)
                Step(instructions[index], ref walk, symbols, saveSlots, calleeContracts, null);
            uint? output = walk.Dead ? null : walk.State;
            if (outStates.TryGetValue(label, out var previous) && previous == output) continue;
            outStates[label] = output;
            if (output is not { } value) continue;
            foreach (var successor in cfg.Successors(label))
            {
                var merged = inStates.TryGetValue(successor, out var existing) ? existing & value : value;
                if (inStates.TryGetValue(successor, out var old) && old == merged && outStates.ContainsKey(successor))
                    continue;
                inStates[successor] = merged;
                if (queued.Add(successor)) worklist.Enqueue(successor);
            }
        }
        return inStates;
    }

    private static void Check(
        IrFunction function,
        IrCfg cfg,
        StackSymbols symbols,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts,
        Dictionary<string, uint> inStates,
        Recorder recorder)
    {
        foreach (var block in function.Blocks)
        {
            if (!inStates.TryGetValue(block.Label, out var state)) continue;
            var walk = new Walk { State = state };
            var instructions = block.Instructions;
            for (var index = 0; index < instructions.Count && !walk.Dead; ++index)
            {
                walk.SaveRestoreRegisters = 0;
                walk.Impure = false;
                var badBefore = walk.Bad;
                Step(instructions[index], ref walk, symbols, recorder.SaveSlots, calleeContracts, recorder);
                if (recorder.Debug && instructions[index] is IrLoad or IrStore)
                    Console.WriteLine($"[nv]   {block.Label}#{index} state=0x{walk.State:X8}: {instructions[index]}");
                if (walk.Bad != badBefore && recorder.Reason is null)
                    recorder.Reason = $"0x{walk.Bad & ~badBefore:X8} at {block.Label}#{index}: {Describe(instructions[index])}";
                if (recorder.Debug && walk.Bad != badBefore)
                    Console.WriteLine($"[nv] {block.Label}#{index} bad+=0x{walk.Bad & ~badBefore:X8} state=0x{walk.State:X8}: {instructions[index]}");
                if (walk.SaveRestoreRegisters != 0 && !walk.Impure)
                    recorder.SaveRestores.Add(new NonvolatileSaveRestore(block.Label, index, walk.SaveRestoreRegisters));
            }
            recorder.Bad |= walk.Bad;
            if (walk.Dead) continue;
            var terminator = instructions.Count == 0 ? null : instructions[^1];
            // Leaving the function (a return or a tail call) hands the caller whatever r14-r31
            // hold now. A recognized jump table's dispatcher default is not an exit: the switch's
            // bounds check keeps every selector on a listed case, and the emitter's default arm
            // publishes the complete register file in case one ever strays.
            var leaves = cfg.Successors(block.Label).Count == 0 || terminator switch
            {
                IrBranch branch => !cfg.Blocks.ContainsKey(branch.TrueLabel) || !cfg.Blocks.ContainsKey(branch.FalseLabel),
                IrJump jump => !cfg.Blocks.ContainsKey(jump.TargetLabel),
                _ => false
            };
            if (leaves)
            {
                if (recorder.Debug && (NonvolatileGprMask & ~walk.State & ~recorder.Bad) != 0)
                    Console.WriteLine($"[nv] exit {block.Label} not restored 0x{NonvolatileGprMask & ~walk.State:X8}");
                if ((NonvolatileGprMask & ~walk.State & ~recorder.Bad) != 0)
                    recorder.Reason ??= $"0x{NonvolatileGprMask & ~walk.State:X8} not restored at exit {block.Label}";
                recorder.Bad |= NonvolatileGprMask & ~walk.State;
            }
        }
    }

    /// <summary>Set to a function name to print which instruction rejects each register.</summary>
    public static string? DebugFunction { get; set; } =
        Environment.GetEnvironmentVariable("MH3_NV_DEBUG") is { Length: > 0 } debug ? debug : null;

    private static void Step(
        IrInstruction instruction,
        ref Walk walk,
        StackSymbols symbols,
        Dictionary<Slot, uint> saveSlots,
        IReadOnlyDictionary<uint, GuestAbiContract>? calleeContracts,
        Recorder? recorder)
    {
        switch (instruction)
        {
            case IrPhi or IrComment or IrTracePpc:
                return;
            case IrUndefined:
                walk.Bad |= NonvolatileGprMask;
                walk.State = 0;
                return;
            case IrStore store:
                StoreHalf(store.Address, store.Source, store.SizeBytes, ref walk, symbols, saveSlots, recorder);
                return;
            case IrResolvedStore store:
                StoreHalf(store.OriginalAddress, store.Source, store.SizeBytes, ref walk, symbols, saveSlots, recorder);
                return;
            case IrResolvedStorePair pair:
                // Each half that is not a save marks the pair impure itself.
                StoreHalf(pair.FirstOriginalAddress, pair.FirstSource, pair.ElementSizeBytes, ref walk, symbols, saveSlots, recorder);
                StoreHalf(pair.SecondOriginalAddress, pair.SecondSource, pair.ElementSizeBytes, ref walk, symbols, saveSlots, recorder);
                return;
            case IrLoad load:
                LoadHalf(load.Destination, load.Address, load.SizeBytes, ref walk, symbols, saveSlots);
                return;
            case IrResolvedLoad load:
                LoadHalf(load.Destination, load.OriginalAddress, load.SizeBytes, ref walk, symbols, saveSlots);
                return;
            case IrResolvedLoadPair pair:
                LoadHalf(pair.FirstDestination, pair.FirstOriginalAddress, pair.ElementSizeBytes, ref walk, symbols, saveSlots);
                LoadHalf(pair.SecondDestination, pair.SecondOriginalAddress, pair.ElementSizeBytes, ref walk, symbols, saveSlots);
                return;
            case IrResolvedPsqStore psqStore:
                Generic(instruction, ref walk);
                if (psqStore.OriginalAddress.RegisterName is { } psqBase &&
                    saveSlots.TryGetValue(symbols.Resolve(psqBase, 0), out var psqClobbered))
                    walk.Bad |= psqClobbered;
                return;
            case IrCall call when GuestTargetParser.TryParseAddress(call.Target, out var target):
                if (TryGetGprThunk(target, out var isSave, out var start))
                {
                    Thunk(isSave, start, ref walk, symbols, saveSlots, recorder);
                    return;
                }
                if (TryGetFprThunk(target))
                {
                    TrackR11(instruction, ref walk);
                    return;
                }
                if (calleeContracts?.TryGetValue(target, out var callee) == true)
                {
                    if ((callee.BoundaryFlags & GuestCallBoundaryFlags.NoReturn) != 0)
                    {
                        walk.Dead = true;
                        return;
                    }
                    walk.Bad |= callee.GprReadBeforeWriteMask & NonvolatileGprMask & walk.State;
                    walk.State &= ~(callee.GprPossibleWriteMask & NonvolatileGprMask);
                }
                // The decoded call's arguments are the volatile ABI registers; only its
                // destination (if any) is a real definition here.
                Define(call.Destination, ref walk);
                return;
            case IrCall helper:
                var effect = GuestHelperEffectCatalog.Analyze(helper);
                walk.Bad |= effect.GprReadMask & NonvolatileGprMask & walk.State;
                Generic(instruction, ref walk);
                walk.State &= ~(effect.GprWriteMask & NonvolatileGprMask);
                return;
            default:
                Generic(instruction, ref walk);
                return;
        }
    }

    private static void Generic(IrInstruction instruction, ref Walk walk)
    {
        foreach (var use in IrRegisterDataFlow.Uses(instruction))
            walk.Bad |= Bit(use) & walk.State;
        foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
            Define(definition, ref walk);
    }

    private static void Define(string? name, ref Walk walk)
    {
        if (string.IsNullOrWhiteSpace(name)) return;
        walk.State &= ~Bit(name);
        if (IsR11(name)) walk.R11 = name;
    }

    private static void TrackR11(IrInstruction instruction, ref Walk walk)
    {
        foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
            Define(definition, ref walk);
    }

    private static void StoreHalf(
        IrAddress address,
        IrValue source,
        int size,
        ref Walk walk,
        StackSymbols symbols,
        Dictionary<Slot, uint> saveSlots,
        Recorder? recorder)
    {
        // The caller's value used as an address is a real read of it.
        walk.Bad |= Bit(address.Base) & walk.State;
        var slot = symbols.Resolve(address.Base, address.Offset);
        var sourceBit = source.RegisterName is { } sourceName ? Bit(sourceName) : 0u;
        if ((sourceBit & walk.State) != 0)
        {
            if (size != 4)
            {
                walk.Bad |= sourceBit;
                walk.Impure = true;
                return;
            }
            if (recorder is not null)
                recorder.NewSaveSlots[slot] = recorder.NewSaveSlots.GetValueOrDefault(slot) | sourceBit;
            if (saveSlots.TryGetValue(slot, out var sharing) && (sharing & ~sourceBit) != 0)
                walk.Bad |= sharing & ~sourceBit;
            walk.SaveRestoreRegisters |= sourceBit;
            return;
        }
        walk.Impure = true;
        if (saveSlots.TryGetValue(slot, out var clobbered))
            walk.Bad |= clobbered;
    }

    private static void LoadHalf(
        string destination,
        IrAddress address,
        int size,
        ref Walk walk,
        StackSymbols symbols,
        Dictionary<Slot, uint> saveSlots)
    {
        walk.Bad |= Bit(address.Base) & walk.State;
        var slot = symbols.Resolve(address.Base, address.Offset);
        var destinationBit = Bit(destination);
        if (saveSlots.TryGetValue(slot, out var saved))
        {
            if (destinationBit != 0 && (saved & destinationBit) != 0 && size == 4)
            {
                walk.State |= destinationBit;
                walk.SaveRestoreRegisters |= destinationBit;
                if (IsR11(destination)) walk.R11 = destination;
                return;
            }
            // Anything else reading a save slot observes the caller's value.
            walk.Bad |= saved;
        }
        walk.Impure = true;
        Define(destination, ref walk);
    }

    /// <summary>
    /// <c>_save_gpr_N</c> stores rK at <c>r11 + (K - 32) * 4</c> for K = N..31;
    /// <c>_rest_gpr_N</c> loads them back.
    /// </summary>
    private static void Thunk(
        bool isSave,
        int start,
        ref Walk walk,
        StackSymbols symbols,
        Dictionary<Slot, uint> saveSlots,
        Recorder? recorder)
    {
        var range = uint.MaxValue << start;
        if (walk.R11 is null)
        {
            // An unknown frame base: the slots can't be matched.
            walk.Bad |= range & NonvolatileGprMask;
            if (!isSave) walk.State &= ~range;
            walk.Impure = true;
            return;
        }
        for (var register = start; register <= 31; ++register)
        {
            var bit = 1u << register;
            var slot = symbols.Resolve(walk.R11, (register - 32) * 4);
            if (isSave)
            {
                if ((walk.State & bit) != 0)
                {
                    if (recorder is not null)
                        recorder.NewSaveSlots[slot] = recorder.NewSaveSlots.GetValueOrDefault(slot) | bit;
                    walk.SaveRestoreRegisters |= bit;
                }
                else
                {
                    walk.Impure = true;
                    if (saveSlots.TryGetValue(slot, out var clobbered)) walk.Bad |= clobbered;
                }
            }
            else if (saveSlots.TryGetValue(slot, out var saved) && (saved & bit) != 0)
            {
                walk.State |= bit;
                walk.SaveRestoreRegisters |= bit;
            }
            else
            {
                walk.Impure = true;
                if (saveSlots.TryGetValue(slot, out var other)) walk.Bad |= other;
                walk.State &= ~bit;
            }
        }
    }

    private static bool TryGetGprThunk(uint address, out bool isSave, out int start)
    {
        var thunks = GuestSaveRestoreThunks.Current;
        if (TryRange(address, thunks.SaveGpr, out start)) { isSave = true; return true; }
        if (TryRange(address, thunks.RestGpr, out start)) { isSave = false; return true; }
        isSave = false;
        return false;
    }

    private static bool TryGetFprThunk(uint address)
    {
        var thunks = GuestSaveRestoreThunks.Current;
        return TryRange(address, thunks.SaveFpr, out _) || TryRange(address, thunks.RestFpr, out _);
    }

    private static bool TryRange(uint candidate, GuestSaveRestoreThunkRange? range, out int start)
    {
        start = -1;
        if (range is null) return false;
        var (baseAddress, firstRegister, lastRegister) = range;
        var lastAddress = baseAddress + checked((uint)((lastRegister - firstRegister) * 4));
        if (candidate < baseAddress || candidate > lastAddress || ((candidate - baseAddress) & 3u) != 0)
            return false;
        start = firstRegister + checked((int)((candidate - baseAddress) / 4));
        return true;
    }

    /// <summary>The r14-r31 bit an SSA register name names, or 0.</summary>
    private static uint Bit(string? name)
    {
        if (string.IsNullOrEmpty(name)) return 0;
        var baseName = RegisterNameUtils.StripNumericSuffixSpan(name.AsSpan());
        if (baseName.Length is < 3 or > 3 || baseName[0] != 'r') return 0;
        if (!int.TryParse(baseName[1..], out var index) || index is < 14 or > 31) return 0;
        return 1u << index;
    }

    private static string Describe(IrInstruction instruction) => instruction switch
    {
        IrCall call => $"call {call.Target} -> {call.Destination}",
        IrIndirectCall => "indirect call",
        _ => instruction.ToString().Replace("Translator.Core.Ir.", string.Empty, StringComparison.Ordinal)
    };

    private static bool IsGprName(string name)
    {
        var baseName = RegisterNameUtils.StripNumericSuffixSpan(name.AsSpan());
        return baseName.Length is 2 or 3 && baseName[0] == 'r' && int.TryParse(baseName[1..], out var index) &&
               index is >= 0 and < 32;
    }

    private static bool IsR11(string name) =>
        RegisterNameUtils.StripNumericSuffixSpan(name.AsSpan()).SequenceEqual("r11");

    /// <summary>
    /// Addresses as a root SSA value plus a constant, following copies, constant adds, phis of
    /// equal addresses and reloads of a stored address (the back chain). Every SSA name without a
    /// definition is the incoming architectural value, so all of one register's share a root.
    /// </summary>
    internal sealed class StackSymbols
    {
        private readonly Dictionary<string, Slot> _values;
        private readonly HashSet<string> _defined;

        private StackSymbols(Dictionary<string, Slot> values, HashSet<string> defined)
        {
            _values = values;
            _defined = defined;
        }

        public Slot Resolve(string name, long offset)
        {
            var root = Root(name);
            return root with { Offset = root.Offset + offset };
        }

        private Slot Root(string name)
        {
            if (_values.TryGetValue(name, out var value)) return value;
            return _defined.Contains(name)
                ? new Slot(name, 0)
                : new Slot("entry:" + RegisterNameUtils.StripNumericSuffix(name), 0);
        }

        /// <summary>Null when the IR is not in SSA form (a name defined twice).</summary>
        public static StackSymbols? Build(IrFunction function)
        {
            var comparer = StringComparer.OrdinalIgnoreCase;
            var defined = new HashSet<string>(comparer);
            foreach (var block in function.Blocks)
                foreach (var instruction in block.Instructions)
                    foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                        // Only GPR names carry addresses. Condition fields such as a
                        // compare's cr0 are not renamed and are defined repeatedly.
                        if (!defined.Add(definition) && IsGprName(definition))
                            return null;

            var symbols = new StackSymbols(new Dictionary<string, Slot>(comparer), defined);
            for (var round = 0; round < 8; ++round)
            {
                var changed = false;
                var stored = new Dictionary<Slot, Slot?>();
                foreach (var block in function.Blocks)
                {
                    foreach (var instruction in block.Instructions)
                    {
                        if (instruction is IrStore { SizeBytes: 4, Source.RegisterName: { } storedName } store)
                            Record(stored, symbols.Resolve(store.Address.Base, store.Address.Offset), symbols.Root(storedName));
                        else if (instruction is IrResolvedStore { SizeBytes: 4, Source.RegisterName: { } resolvedName } resolved)
                            Record(stored, symbols.Resolve(resolved.OriginalAddress.Base, resolved.OriginalAddress.Offset), symbols.Root(resolvedName));
                    }
                }
                foreach (var block in function.Blocks)
                {
                    foreach (var instruction in block.Instructions)
                    {
                        Slot? value = instruction switch
                        {
                            IrAssign { Value.RegisterName: { } source } => symbols.Root(source),
                            IrBinary { Op: "add", Left.RegisterName: { } left, Right.Constant: { } constant } =>
                                symbols.Resolve(left, constant),
                            IrBinary { Op: "add", Left.Constant: { } constant, Right.RegisterName: { } right } =>
                                symbols.Resolve(right, constant),
                            IrPhi phi => SameRoot(symbols, phi),
                            IrLoad { SizeBytes: 4 } load =>
                                stored.GetValueOrDefault(symbols.Resolve(load.Address.Base, load.Address.Offset)),
                            IrResolvedLoad { SizeBytes: 4 } load =>
                                stored.GetValueOrDefault(symbols.Resolve(load.OriginalAddress.Base, load.OriginalAddress.Offset)),
                            _ => null
                        };
                        if (value is not { } slot) continue;
                        var destination = instruction switch
                        {
                            IrAssign assign => assign.Destination,
                            IrBinary binary => binary.Destination,
                            IrPhi phi => phi.Destination,
                            IrLoad load => load.Destination,
                            IrResolvedLoad load => load.Destination,
                            _ => null
                        };
                        if (destination is null || slot.Root == destination) continue;
                        if (symbols._values.TryGetValue(destination, out var previous) && previous == slot) continue;
                        symbols._values[destination] = slot;
                        changed = true;
                    }
                }
                if (!changed) break;
            }
            return symbols;

            static void Record(Dictionary<Slot, Slot?> stored, Slot at, Slot value)
            {
                if (!stored.TryGetValue(at, out var previous)) stored[at] = value;
                else if (previous != value) stored[at] = null;
            }

            static Slot? SameRoot(StackSymbols symbols, IrPhi phi)
            {
                Slot? common = null;
                foreach (var source in phi.Sources.Values)
                {
                    if (string.Equals(source, phi.Destination, StringComparison.OrdinalIgnoreCase)) continue;
                    var value = symbols.Root(source);
                    if (common is null) common = value;
                    else if (common != value) return null;
                }
                return common;
            }
        }
    }
}
