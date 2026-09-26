using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

/// <summary>
/// Coalesces consecutive statically-known gather-pipe stores into one <c>GX_HLE_FIFO_WriteBurst</c>
/// call instead of one GX_HLE_FIFO_Write* per field, avoiding a display-list state re-walk each time.
/// </summary>
public sealed partial class CxxLinearCodeGenerator
{
    // Below this the saved call overhead does not pay for the buffer traffic
    // and the extra stack slot.
    private const int MinimumGpuFifoBurstStores = 4;

    private readonly record struct GpuFifoStoreInfo(bool IsFloat, int ByteLength);

    private sealed record GpuFifoBurstRun(string BufferName, int TotalBytes);

    private sealed record GpuFifoBurstSlot(
        GpuFifoBurstRun Run,
        int ByteOffset,
        GpuFifoStoreInfo Store);

    private sealed class GpuFifoBurstPlan
    {
        public static readonly GpuFifoBurstPlan Empty = new(
            new Dictionary<(string Block, int Index), GpuFifoBurstSlot>(),
            new Dictionary<(string Block, int Index), GpuFifoBurstRun>(),
            Array.Empty<GpuFifoBurstRun>());

        private readonly IReadOnlyDictionary<(string Block, int Index), GpuFifoBurstSlot> _slots;
        private readonly IReadOnlyDictionary<(string Block, int Index), GpuFifoBurstRun> _completions;

        public GpuFifoBurstPlan(
            IReadOnlyDictionary<(string Block, int Index), GpuFifoBurstSlot> slots,
            IReadOnlyDictionary<(string Block, int Index), GpuFifoBurstRun> completions,
            IReadOnlyList<GpuFifoBurstRun> runs)
        {
            _slots = slots;
            _completions = completions;
            Runs = runs;
        }

        public IReadOnlyList<GpuFifoBurstRun> Runs { get; }

        public GpuFifoBurstSlot? Slot(string block, int index) =>
            _slots.Count != 0 && _slots.TryGetValue((block, index), out var slot) ? slot : null;

        public GpuFifoBurstRun? Completion(string block, int index) =>
            _completions.Count != 0 && _completions.TryGetValue((block, index), out var run) ? run : null;
    }

    // The store currently being emitted, if it belongs to a burst run; set around its
    // EmitInstruction call. Thread-static because translation runs Parallel.For over functions.
    [ThreadStatic]
    private static GpuFifoBurstSlot? _activeGpuFifoBurstSlot;

    /// <summary>
    /// Describes a store the emitter can turn into a direct GX_HLE_FIFO_Write* call; shared by the
    /// burst planner and direct emitter so a rejected store can't leave a hole in a burst buffer.
    /// </summary>
    private static bool TryGetKnownGpuFifoStore(
        IrStore store,
        bool isFloatStore,
        IReadOnlyDictionary<string, uint> knownConstants,
        out GpuFifoStoreInfo info)
    {
        if (!TryGetKnownEffectiveAddress(store.Address, knownConstants, out var address) ||
            !IsGpuFifoAddress(address))
        {
            info = default;
            return false;
        }

        if (isFloatStore)
        {
            if (store.SizeBytes != 4)
            {
                info = default;
                return false;
            }

            info = new GpuFifoStoreInfo(IsFloat: true, ByteLength: 4);
            return true;
        }

        if (store.SizeBytes is not (1 or 2 or 4))
        {
            info = default;
            return false;
        }

        info = new GpuFifoStoreInfo(IsFloat: false, ByteLength: store.SizeBytes);
        return true;
    }

    private static bool TryClassifyKnownGpuFifoStore(
        IrInstruction instruction,
        IReadOnlyDictionary<string, uint> knownConstants,
        out GpuFifoStoreInfo info)
    {
        if (instruction is not IrStore store)
        {
            info = default;
            return false;
        }

        var isFloatStore = store.Source.Kind == "register" &&
                           store.Source.RegisterName is not null &&
                           IsFloatRegister(store.Source.RegisterName);
        return TryGetKnownGpuFifoStore(store, isFloatStore, knownConstants, out info);
    }

    /// <summary>
    /// Instructions a burst run may span: pure register/flag computation that can't read the FIFO,
    /// fault, or observe the deferred write. Anything else (memory access, calls, control transfer) ends the run.
    /// </summary>
    private static bool IsGpuFifoBurstTransparent(IrInstruction instruction) =>
        instruction is IrAssign or IrBinary or IrComment or IrTracePpc or IrPhi or IrSetCrField;

    private static GpuFifoBurstPlan BuildGpuFifoBurstPlan(
        IrFunction function,
        IReadOnlyDictionary<string, uint> knownConstants,
        bool enabled)
    {
        if (!enabled) return GpuFifoBurstPlan.Empty;

        var slots = new Dictionary<(string Block, int Index), GpuFifoBurstSlot>();
        var completions = new Dictionary<(string Block, int Index), GpuFifoBurstRun>();
        var runs = new List<GpuFifoBurstRun>();

        foreach (var block in function.Blocks)
        {
            var instructions = block.Instructions;
            var index = 0;
            while (index < instructions.Count)
            {
                if (!TryClassifyKnownGpuFifoStore(instructions[index], knownConstants, out var firstStore))
                {
                    index++;
                    continue;
                }

                var members = new List<(int Index, GpuFifoStoreInfo Store)> { (index, firstStore) };
                var scan = index + 1;
                while (scan < instructions.Count)
                {
                    if (TryClassifyKnownGpuFifoStore(instructions[scan], knownConstants, out var store))
                    {
                        members.Add((scan, store));
                        scan++;
                        continue;
                    }
                    if (IsGpuFifoBurstTransparent(instructions[scan]))
                    {
                        scan++;
                        continue;
                    }
                    break;
                }

                if (members.Count >= MinimumGpuFifoBurstStores)
                {
                    var total = members.Sum(static member => member.Store.ByteLength);
                    var run = new GpuFifoBurstRun($"mkw_fifo_burst_{runs.Count}", total);
                    runs.Add(run);
                    var offset = 0;
                    foreach (var (memberIndex, store) in members)
                    {
                        slots[(block.Label, memberIndex)] = new GpuFifoBurstSlot(run, offset, store);
                        offset += store.ByteLength;
                    }
                    completions[(block.Label, members[^1].Index)] = run;
                }

                // Every store between the run's first member and `scan` is
                // already accounted for, so no later run can start before it.
                index = Math.Max(scan, members[^1].Index + 1);
            }
        }

        return runs.Count == 0
            ? GpuFifoBurstPlan.Empty
            : new GpuFifoBurstPlan(slots, completions, runs);
    }

    /// <summary>
    /// Serializes one store's value into its slot, big-endian, matching the bytes the corresponding
    /// GX_HLE_FIFO_Write* call would push. Evaluated once into a temporary so multi-byte members don't re-evaluate per byte.
    /// </summary>
    private static void EmitGpuFifoBurstSlotStore(
        StringBuilder sb,
        string pad,
        GpuFifoBurstSlot slot,
        string srcExpr)
    {
        var buffer = slot.Run.BufferName;
        if (!slot.Store.IsFloat && slot.Store.ByteLength == 1)
        {
            sb.AppendLine($"{pad}{buffer}[{slot.ByteOffset}] = static_cast<uint8_t>({srcExpr});");
            return;
        }

        var word = slot.Store.IsFloat
            ? $"PpcBitCastToU32Inline(static_cast<float>({srcExpr}))"
            : $"static_cast<uint32_t>({srcExpr})";
        sb.AppendLine($"{pad}{{");
        sb.AppendLine($"{pad}    const uint32_t mkw_fifo_word = {word};");
        for (var byteIndex = 0; byteIndex < slot.Store.ByteLength; byteIndex++)
        {
            var shift = (slot.Store.ByteLength - 1 - byteIndex) * 8;
            var shifted = shift == 0 ? "mkw_fifo_word" : $"(mkw_fifo_word >> {shift})";
            sb.AppendLine($"{pad}    {buffer}[{slot.ByteOffset + byteIndex}] = static_cast<uint8_t>({shifted});");
        }
        sb.AppendLine($"{pad}}}");
    }
}
