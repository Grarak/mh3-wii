using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Forwards the two words an <c>lfd</c> reads back from a stack slot straight from the
    /// <c>stw</c>s that wrote them. That is CodeWarrior's integer-to-float idiom (<c>lis
    /// r0,0x4330; stw r0,8(r1); stw rX,12(r1); lfd f0,8(r1)</c>, then a subtract of the bias):
    /// through guest memory it costs byte swaps and a 64-bit load of two 32-bit stores, which
    /// Cortex-A9 cannot forward, so the load waits for both stores to commit. The high word is
    /// typically stored once per function and the low word in a loop, so the stores are found
    /// by a forward must-analysis of the 4-byte stack slots: each store's value is captured in
    /// a temp at the store, and a load whose both words come from one known store on every
    /// path becomes a double assembled from the two temps. The stores themselves stay.
    /// Guest calls, PSQ and other-size stack stores kill the slots they may write; stores
    /// through addresses not proven to be on the stack are assumed not to alias the frame, as
    /// the fctiw forwarding does.
    /// </summary>
    private static IrFunction ForwardStackWordsToDoubleLoads(IrFunction func, RepresentationEnvironment types)
    {
        var stackFacts = StackAddressFacts.Build(func);
        var hasCandidate = false;
        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is IrLoad { SizeBytes: 8 } load &&
                    IsFloatRegister(load.Destination) &&
                    stackFacts.TryResolve(load.Address, out _))
                {
                    hasCandidate = true;
                    break;
                }
            }
            if (hasCandidate) break;
        }
        if (!hasCandidate) return func;

        var cfg = IrCfg.Build(func);
        var siteIds = new Dictionary<(string Block, int Index), int>();
        var nextSite = 0;

        // Block exit states: 4-byte stack offset -> id of the store site whose value it holds.
        // null is "not computed yet" (the lattice top).
        var exitStates = new Dictionary<string, Dictionary<int, int>?>(StringComparer.Ordinal);
        foreach (var block in func.Blocks) exitStates[block.Label] = null;

        Dictionary<int, int> EntryState(string label)
        {
            if (string.Equals(label, func.EntryLabel, StringComparison.Ordinal))
                return new Dictionary<int, int>();
            Dictionary<int, int>? state = null;
            foreach (var predecessor in cfg.Predecessors(label))
            {
                if (!exitStates.TryGetValue(predecessor, out var predecessorState) || predecessorState is null)
                    continue;
                if (state is null)
                {
                    state = new Dictionary<int, int>(predecessorState);
                    continue;
                }
                foreach (var offset in state.Keys.ToList())
                {
                    if (!predecessorState.TryGetValue(offset, out var site) || site != state[offset])
                        state.Remove(offset);
                }
            }
            return state ?? new Dictionary<int, int>();
        }

        void Kill(Dictionary<int, int> state, int offset, int size)
        {
            foreach (var slot in state.Keys.ToList())
            {
                if (RangesOverlap(slot, 4, offset, size))
                    state.Remove(slot);
            }
        }

        // Applies one instruction; reports a forwardable load as (high site, low site).
        (int High, int Low)? Transfer(Dictionary<int, int> state, string label, int index, IrInstruction instruction)
        {
            switch (instruction)
            {
                case IrLoad { SizeBytes: 8 } load when
                    IsFloatRegister(load.Destination) &&
                    stackFacts.TryResolve(load.Address, out var loadOffset) &&
                    state.TryGetValue(loadOffset, out var high) &&
                    state.TryGetValue(loadOffset + 4, out var low):
                    return (high, low);

                case IrStore store when stackFacts.TryResolve(store.Address, out var storeOffset):
                    Kill(state, storeOffset, store.SizeBytes);
                    if (store.SizeBytes == 4 &&
                        (store.Source.Kind != "register" ||
                         store.Source.RegisterName is not { } source ||
                         !IsFloatRegister(source)))
                    {
                        if (!siteIds.TryGetValue((label, index), out var site))
                        {
                            site = nextSite++;
                            siteIds[(label, index)] = site;
                        }
                        state[storeOffset] = site;
                    }
                    return null;

                case IrCall call when TryGetPsqStackAccess(call, stackFacts, out var psq):
                    if (psq.IsStore) Kill(state, psq.Offset, psq.SizeBytes);
                    return null;

                case IrCall call when string.IsNullOrWhiteSpace(call.Destination) || IsGuestCallTarget(call.Target):
                    state.Clear();
                    return null;

                case IrIndirectCall:
                case IrIndirectJump:
                case IrUndefined:
                    state.Clear();
                    return null;

                default:
                    return null;
            }
        }

        // Fixpoint over the CFG in block order.
        var changedState = true;
        var rounds = 0;
        while (changedState && rounds++ < 64)
        {
            changedState = false;
            foreach (var block in func.Blocks)
            {
                var state = EntryState(block.Label);
                for (var index = 0; index < block.Instructions.Count; index++)
                    Transfer(state, block.Label, index, block.Instructions[index]);
                var previous = exitStates[block.Label];
                if (previous is null || previous.Count != state.Count ||
                    previous.Any(entry => !state.TryGetValue(entry.Key, out var site) || site != entry.Value))
                {
                    exitStates[block.Label] = state;
                    changedState = true;
                }
            }
        }
        if (changedState) return func; // did not converge: leave the function alone

        // Final pass: find the forwardable loads and the store sites they use.
        var forwards = new Dictionary<(string Block, int Index), (int High, int Low)>();
        foreach (var block in func.Blocks)
        {
            var state = EntryState(block.Label);
            for (var index = 0; index < block.Instructions.Count; index++)
            {
                if (Transfer(state, block.Label, index, block.Instructions[index]) is { } pair)
                    forwards[(block.Label, index)] = pair;
            }
        }
        if (forwards.Count == 0) return func;

        var usedSites = new HashSet<int>(forwards.Values.SelectMany(pair => new[] { pair.High, pair.Low }));
        string SiteTemp(int site) => $"stackword{site}";
        foreach (var site in usedSites) types.Unify(SiteTemp(site), ValueRepresentation.UInt32);

        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);
        foreach (var block in func.Blocks)
        {
            var instructions = new List<IrInstruction>(block.Instructions.Count + 2);
            for (var index = 0; index < block.Instructions.Count; index++)
            {
                var instruction = block.Instructions[index];
                if (instruction is IrStore store &&
                    siteIds.TryGetValue((block.Label, index), out var site) &&
                    usedSites.Contains(site))
                {
                    instructions.Add(new IrAssign(SiteTemp(site), store.Source));
                }
                if (forwards.TryGetValue((block.Label, index), out var pair))
                {
                    var load = (IrLoad)instruction;
                    instructions.Add(new IrBinary(
                        load.Destination,
                        IrValue.Register(SiteTemp(pair.High)),
                        IrValue.Register(SiteTemp(pair.Low)),
                        "words_to_f64"));
                    continue;
                }
                instructions.Add(instruction);
            }
            rewrittenBlocks.Add(new IrBasicBlock(block.Label, instructions));
        }

        return new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks);
    }
}
