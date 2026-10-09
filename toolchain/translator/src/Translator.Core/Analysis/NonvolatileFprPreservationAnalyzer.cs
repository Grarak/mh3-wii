using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Slot = Translator.Core.Analysis.NonvolatileGprPreservationAnalyzer.Slot;
using StackSymbols = Translator.Core.Analysis.NonvolatileGprPreservationAnalyzer.StackSymbols;

namespace Translator.Core.Analysis;

/// <summary>
/// Which of f14-f31 a function hands back as it got them, as the PowerPC EABI promises: at every
/// exit the register holds the caller's value, because the function never changed it or reloaded
/// it (an 8-byte load, <c>_rest_fpr_N</c>) from the stack slot where it saved that value (an
/// 8-byte store, <c>_save_fpr_N</c>) and where nothing else was written. Both move the raw 64
/// bits, so the caller gets the exact value back, and the dispatch need not save and restore that
/// register around the call. Calls are taken to keep f14-f31, as the function's own write mask
/// takes them: a callee that may not is guarded at its call.
/// </summary>
public static class NonvolatileFprPreservationAnalyzer
{
    public const uint NonvolatileFprMask = 0xFFFFC000u;

    private sealed class Recorder
    {
        public Recorder(Dictionary<Slot, uint> saveSlots) => SaveSlots = saveSlots;
        public Dictionary<Slot, uint> SaveSlots { get; }
        public Dictionary<Slot, uint> NewSaveSlots { get; } = new();
        public uint Bad;
    }

    private struct Walk
    {
        /// <summary>f14-f31 holding the caller's value now.</summary>
        public uint State;
        /// <summary>SSA name r11 last took in this block, the save/restore thunks' frame base.</summary>
        public string? R11;
        public uint Bad;
    }

    /// <summary>The f14-f31 bits every exit of <paramref name="function"/> keeps; 0 when unknown.</summary>
    public static uint PreservedMask(IrFunction function)
    {
        if (function.Blocks.Count == 0) return 0;
        var symbols = StackSymbols.Build(function);
        if (symbols is null) return 0;
        var cfg = IrCfg.Build(function);
        var saveSlots = new Dictionary<Slot, uint>();

        for (var round = 0; round < 6; ++round)
        {
            var inStates = Solve(cfg, symbols, saveSlots);
            var recorder = new Recorder(saveSlots);
            Check(function, cfg, symbols, inStates, recorder);
            if (SameSlots(recorder.NewSaveSlots, saveSlots))
                return NonvolatileFprMask & ~recorder.Bad;
            saveSlots = recorder.NewSaveSlots;
        }

        return 0;
    }

    private static bool SameSlots(Dictionary<Slot, uint> left, Dictionary<Slot, uint> right) =>
        left.Count == right.Count &&
        left.All(pair => right.TryGetValue(pair.Key, out var mask) && mask == pair.Value);

    private static Dictionary<string, uint> Solve(IrCfg cfg, StackSymbols symbols, Dictionary<Slot, uint> saveSlots)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var inStates = new Dictionary<string, uint>(comparer) { [cfg.Entry] = NonvolatileFprMask };
        var outStates = new Dictionary<string, uint>(comparer);
        var worklist = new Queue<string>();
        var queued = new HashSet<string>(comparer) { cfg.Entry };
        worklist.Enqueue(cfg.Entry);

        while (worklist.Count > 0)
        {
            var label = worklist.Dequeue();
            queued.Remove(label);
            var walk = new Walk { State = inStates[label] };

            foreach (var instruction in cfg.Blocks[label].Instructions)
                Step(instruction, ref walk, symbols, saveSlots, null);

            if (outStates.TryGetValue(label, out var previous) && previous == walk.State) continue;
            outStates[label] = walk.State;

            foreach (var successor in cfg.Successors(label))
            {
                var merged = inStates.TryGetValue(successor, out var existing) ? existing & walk.State : walk.State;
                if (inStates.TryGetValue(successor, out var old) && old == merged && outStates.ContainsKey(successor))
                    continue;
                inStates[successor] = merged;
                if (queued.Add(successor)) worklist.Enqueue(successor);
            }
        }

        return inStates;
    }

    private static void Check(IrFunction function, IrCfg cfg, StackSymbols symbols, Dictionary<string, uint> inStates, Recorder recorder)
    {
        foreach (var block in function.Blocks)
        {
            if (!inStates.TryGetValue(block.Label, out var state)) continue;
            var walk = new Walk { State = state };

            foreach (var instruction in block.Instructions)
                Step(instruction, ref walk, symbols, recorder.SaveSlots, recorder);

            recorder.Bad |= walk.Bad;
            var instructions = block.Instructions;
            var terminator = instructions.Count == 0 ? null : instructions[^1];

            // Leaving the function (a return or a tail call) hands the caller whatever f14-f31
            // hold now; as in NonvolatileGprPreservationAnalyzer, a jump table's default is no exit.
            var leaves = cfg.Successors(block.Label).Count == 0 || terminator switch
            {
                IrBranch branch => !cfg.Blocks.ContainsKey(branch.TrueLabel) || !cfg.Blocks.ContainsKey(branch.FalseLabel),
                IrJump jump => !cfg.Blocks.ContainsKey(jump.TargetLabel),
                _ => false
            };

            if (leaves)
                recorder.Bad |= NonvolatileFprMask & ~walk.State;
        }
    }

    private static void Step(IrInstruction instruction, ref Walk walk, StackSymbols symbols, Dictionary<Slot, uint> saveSlots, Recorder? recorder)
    {
        switch (instruction)
        {
            case IrPhi or IrComment or IrTracePpc:
                return;
            case IrUndefined:
                walk.Bad |= NonvolatileFprMask;
                walk.State = 0;
                return;
            case IrStore store:
                Store(symbols.Resolve(store.Address.Base, store.Address.Offset), store.Source, store.SizeBytes, ref walk, saveSlots, recorder);
                return;
            case IrResolvedStore store:
                Store(symbols.Resolve(store.OriginalAddress.Base, store.OriginalAddress.Offset), store.Source, store.SizeBytes, ref walk, saveSlots, recorder);
                return;
            case IrResolvedStorePair pair:
                Store(symbols.Resolve(pair.FirstOriginalAddress.Base, pair.FirstOriginalAddress.Offset), pair.FirstSource, pair.ElementSizeBytes, ref walk, saveSlots, recorder);
                Store(symbols.Resolve(pair.SecondOriginalAddress.Base, pair.SecondOriginalAddress.Offset), pair.SecondSource, pair.ElementSizeBytes, ref walk, saveSlots, recorder);
                return;
            case IrResolvedPsqStore psqStore:
                // psq_st w=1 stores one value, w=0 two: at most 8 bytes.
                walk.Bad |= Written(psqStore.OriginalAddress, 8, symbols, saveSlots);
                return;
            case IrLoad load:
                Load(load.Destination, symbols.Resolve(load.Address.Base, load.Address.Offset), load.SizeBytes, ref walk, saveSlots);
                return;
            case IrResolvedLoad load:
                Load(load.Destination, symbols.Resolve(load.OriginalAddress.Base, load.OriginalAddress.Offset), load.SizeBytes, ref walk, saveSlots);
                return;
            case IrResolvedLoadPair pair:
                Load(pair.FirstDestination, symbols.Resolve(pair.FirstOriginalAddress.Base, pair.FirstOriginalAddress.Offset), pair.ElementSizeBytes, ref walk, saveSlots);
                Load(pair.SecondDestination, symbols.Resolve(pair.SecondOriginalAddress.Base, pair.SecondOriginalAddress.Offset), pair.ElementSizeBytes, ref walk, saveSlots);
                return;
            case IrCall call when GuestTargetParser.TryParseAddress(call.Target, out var target):
                Thunk(target, ref walk, symbols, saveSlots, recorder);
                Define(call.Destination, ref walk);
                return;
            case IrCall helper:
                Helper(helper, ref walk, symbols, saveSlots);
                foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                    Define(definition, ref walk);
                return;
            default:
                foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                    Define(definition, ref walk);
                return;
        }
    }

    /// <summary>The helpers that write guest memory: the save slots they may overwrite.</summary>
    private static void Helper(IrCall helper, ref Walk walk, StackSymbols symbols, Dictionary<Slot, uint> saveSlots)
    {
        var target = helper.Target;

        if (target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase) && helper.Arguments.Count > 0)
        {
            walk.Bad |= Written(helper.Arguments[0], 8, symbols, saveSlots);
        }
        else if (target.Equals("memset_zero_32", StringComparison.OrdinalIgnoreCase) && helper.Arguments.Count > 0)
        {
            // dcbz, on the 32-byte block its address was rounded down to.
            walk.Bad |= Written(helper.Arguments[0], 32, symbols, saveSlots);
        }
        else if (target.Equals("OSSystemCall", StringComparison.OrdinalIgnoreCase))
        {
            walk.Bad |= NonvolatileFprMask;
            walk.State = 0;
        }
        else if (target.StartsWith("PPC_St", StringComparison.OrdinalIgnoreCase) ||
                 target.Equals("PPC_Ecowx", StringComparison.OrdinalIgnoreCase))
        {
            walk.Bad |= AllSaved(saveSlots);
        }
    }

    private static void Define(string? name, ref Walk walk)
    {
        if (string.IsNullOrWhiteSpace(name)) return;
        walk.State &= ~Bit(name);
        if (RegisterNameUtils.StripNumericSuffixSpan(name.AsSpan()).SequenceEqual("r11")) walk.R11 = name;
    }

    private static void Store(Slot slot, IrValue source, int size, ref Walk walk, Dictionary<Slot, uint> saveSlots, Recorder? recorder)
    {
        var bit = source.RegisterName is { } name ? Bit(name) : 0u;

        if (size == 8 && (bit & walk.State) != 0)
        {
            // stfd of the caller's value: a save slot of it.
            if (recorder is not null)
                recorder.NewSaveSlots[slot] = recorder.NewSaveSlots.GetValueOrDefault(slot) | bit;
            walk.Bad |= Overlapping(slot, size, saveSlots) & ~bit;
            return;
        }

        walk.Bad |= Overlapping(slot, size, saveSlots);
    }

    private static void Load(string destination, Slot slot, int size, ref Walk walk, Dictionary<Slot, uint> saveSlots)
    {
        var bit = Bit(destination);

        if (bit != 0 && size == 8 && saveSlots.TryGetValue(slot, out var saved) && (saved & bit) != 0)
        {
            walk.State |= bit;
            return;
        }

        Define(destination, ref walk);
    }

    /// <summary>The save slots a store of <paramref name="size"/> bytes at <paramref name="address"/> may overwrite.</summary>
    private static uint Written(IrValue address, int size, StackSymbols symbols, Dictionary<Slot, uint> saveSlots) =>
        address.RegisterName is { } name ? Overlapping(symbols.Resolve(name, 0), size, saveSlots) : AllSaved(saveSlots);

    private static uint AllSaved(Dictionary<Slot, uint> saveSlots) =>
        saveSlots.Values.Aggregate(0u, (all, registers) => all | registers);

    /// <summary>The registers saved in the slots that <paramref name="size"/> bytes at <paramref name="slot"/> overlap.</summary>
    private static uint Overlapping(Slot slot, int size, Dictionary<Slot, uint> saveSlots)
    {
        uint mask = 0;

        foreach (var (saved, registers) in saveSlots)
            if (string.Equals(saved.Root, slot.Root, StringComparison.OrdinalIgnoreCase) &&
                slot.Offset < saved.Offset + 8 && saved.Offset < slot.Offset + size)
                mask |= registers;

        return mask;
    }

    /// <summary>
    /// <c>_save_fpr_N</c> stores fK at <c>r11 + (K - 32) * 8</c> for K = N..31 and
    /// <c>_rest_fpr_N</c> loads them back; <c>_save_gpr_N</c> stores rK at <c>r11 + (K - 32) * 4</c>.
    /// Other calls keep f14-f31.
    /// </summary>
    private static void Thunk(uint target, ref Walk walk, StackSymbols symbols, Dictionary<Slot, uint> saveSlots, Recorder? recorder)
    {
        var thunks = GuestSaveRestoreThunks.Current;

        if (TryRange(target, thunks.SaveGpr, out var gprStart))
        {
            for (var register = gprStart; register <= 31; ++register)
                walk.Bad |= walk.R11 is null ? AllSaved(saveSlots) : Overlapping(symbols.Resolve(walk.R11, (register - 32) * 4), 4, saveSlots);
            return;
        }

        var isSave = TryRange(target, thunks.SaveFpr, out var start);
        if (!isSave && !TryRange(target, thunks.RestFpr, out start)) return;

        if (walk.R11 is not { } frame)
        {
            // An unknown frame base: no slot can be matched.
            if (isSave) walk.Bad |= AllSaved(saveSlots);
            else walk.State &= ~(NonvolatileFprMask & (uint.MaxValue << start));
            return;
        }

        for (var register = Math.Max(start, 14); register <= 31; ++register)
        {
            var bit = 1u << register;
            var slot = symbols.Resolve(frame, (register - 32) * 8);

            if (isSave)
                Store(slot, IrValue.Register($"f{register}"), 8, ref walk, saveSlots, recorder);
            else if (saveSlots.TryGetValue(slot, out var saved) && (saved & bit) != 0)
                walk.State |= bit;
            else
                walk.State &= ~bit;
        }
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

    /// <summary>The f14-f31 bit an SSA register name names, or 0.</summary>
    private static uint Bit(string? name)
    {
        if (string.IsNullOrEmpty(name)) return 0;
        var baseName = RegisterNameUtils.StripNumericSuffixSpan(name.AsSpan());
        if (baseName.Length != 3 || baseName[0] != 'f') return 0;
        if (!int.TryParse(baseName[1..], out var index) || index is < 14 or > 31) return 0;
        return 1u << index;
    }
}
