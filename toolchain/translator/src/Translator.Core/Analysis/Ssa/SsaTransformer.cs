using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using Translator.Core;
using Translator.Core.Ir;

namespace Translator.Core.Analysis.Ssa;

/// <summary>
/// Converts an untyped IR function into SSA form with explicit phi nodes and performs
/// a basic use-def validation. The implementation is intentionally conservative and
/// works on the lightweight IR used in early translator phases.
/// </summary>
public sealed class SsaTransformer
{
    public SsaResult Convert(IrFunction function)
    {
        if (function.Blocks.Count == 0)
        {
            throw new ArgumentException("Function must contain at least one basic block.", nameof(function));
        }

        var cfg = IrCfg.Build(function);
        var dominators = ComputeDominators(cfg);
        var idom = ComputeImmediateDominators(dominators);
        var dominanceFrontier = ComputeDominanceFrontier(cfg, dominators, idom);

        // Insert minimal phi nodes based on dominance frontier of definitions.
        var withPhis = InsertPhiNodes(function, cfg, dominanceFrontier);

        // Rename to SSA.
        var renamed = RenameToSsa(withPhis, cfg, idom);

        return new SsaResult(renamed, cfg);
    }

    /// <summary>
    /// Dominator sets kept as bitsets indexed by block ordinal. Expanding them into
    /// <c>Dictionary&lt;string, HashSet&lt;string&gt;&gt;</c> costs O(B^2) string hashing and
    /// allocation for no benefit, so every consumer works on the bitsets directly.
    /// </summary>
    private readonly struct DominatorBits
    {
        public DominatorBits(List<string> blocks, Dictionary<string, int> index, ulong[][] bits, int words)
        {
            Blocks = blocks;
            Index = index;
            Bits = bits;
            Words = words;
            LastWordMask = blocks.Count % 64 == 0 ? ulong.MaxValue : (1UL << (blocks.Count % 64)) - 1;
        }

        /// <summary>Block labels in CFG enumeration order; the ordinal of a block is its position here.</summary>
        public List<string> Blocks { get; }

        public Dictionary<string, int> Index { get; }

        public ulong[][] Bits { get; }

        public int Words { get; }

        /// <summary>Valid bits of the final word; the solver leaves padding bits set.</summary>
        public ulong LastWordMask { get; }

        /// <summary>True when <paramref name="dominator"/> belongs to the dominator set of <paramref name="block"/>.</summary>
        public bool Dominates(int dominator, int block) =>
            (Bits[block][dominator / 64] & (1UL << (dominator % 64))) != 0;

        /// <summary>Word <paramref name="word"/> of a dominator set with padding bits cleared.</summary>
        public ulong Word(int block, int word) =>
            word == Words - 1 ? Bits[block][word] & LastWordMask : Bits[block][word];
    }

    private static DominatorBits ComputeDominators(IrCfg cfg)
    {
        // Use a more efficient worklist-based approach
        var comparer = StringComparer.OrdinalIgnoreCase;
        var blockList = cfg.Blocks.Keys.ToList();
        var blockIndex = blockList.Select((b, i) => (b, i)).ToDictionary(x => x.b, x => x.i, comparer);
        var n = blockList.Count;
        
        // Use bit sets for faster intersection
        var domBits = new ulong[n][];
        var wordsNeeded = (n + 63) / 64;
        for (var i = 0; i < n; i++)
        {
            domBits[i] = new ulong[wordsNeeded];
            // Initialize all blocks to dominate by all (all bits set)
            for (var w = 0; w < wordsNeeded; w++)
            {
                domBits[i][w] = ulong.MaxValue;
            }
        }
        
        // Entry dominates only itself
        var entryIdx = blockIndex[cfg.Entry];
        Array.Clear(domBits[entryIdx]);
        domBits[entryIdx][entryIdx / 64] = 1UL << (entryIdx % 64);

        var changed = true;
        var temp = new ulong[wordsNeeded];
        while (changed)
        {
            changed = false;
            for (var i = 0; i < n; i++)
            {
                var block = blockList[i];
                if (comparer.Equals(block, cfg.Entry))
                {
                    continue;
                }

                var preds = cfg.Predecessors(block);
                if (preds.Count == 0)
                {
                    // Unreachable block - it only dominates itself. Only report
                    // a change until the bitset reaches that fixed point.
                    var differsFromSelfOnly = false;
                    for (var w = 0; w < wordsNeeded; w++)
                    {
                        var expected = w == i / 64 ? 1UL << (i % 64) : 0UL;
                        if (domBits[i][w] != expected)
                        {
                            differsFromSelfOnly = true;
                            break;
                        }
                    }
                    if (differsFromSelfOnly)
                    {
                        Array.Clear(domBits[i]);
                        domBits[i][i / 64] |= 1UL << (i % 64);
                        changed = true;
                    }
                    continue;
                }

                // Compute intersection of all predecessors
                var firstPredIdx = blockIndex[preds[0]];
                Array.Copy(domBits[firstPredIdx], temp, wordsNeeded);
                for (var p = 1; p < preds.Count; p++)
                {
                    var predIdx = blockIndex[preds[p]];
                    for (var w = 0; w < wordsNeeded; w++)
                    {
                        temp[w] &= domBits[predIdx][w];
                    }
                }
                
                // Add self
                temp[i / 64] |= 1UL << (i % 64);

                // Check if changed
                for (var w = 0; w < wordsNeeded; w++)
                {
                    if (domBits[i][w] != temp[w])
                    {
                        changed = true;
                        Array.Copy(temp, domBits[i], wordsNeeded);
                        break;
                    }
                }
            }
        }

        return new DominatorBits(blockList, blockIndex, domBits, wordsNeeded);
    }

    private static Dictionary<string, string?> ComputeImmediateDominators(DominatorBits dominators)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var idom = new Dictionary<string, string?>(comparer);
        var blocks = dominators.Blocks;
        var n = blocks.Count;
        var words = dominators.Words;

        // Cardinality of every dominator set. The strict dominators of a block are totally
        // ordered by dominance, so the immediate dominator is exactly the strict dominator
        // with the largest dominator set - no O(B) "dominates nobody else" scan per candidate.
        var domSize = new int[n];
        for (var i = 0; i < n; i++)
        {
            var size = 0;
            for (var w = 0; w < words; w++)
            {
                size += BitOperations.PopCount(dominators.Word(i, w));
            }

            domSize[i] = size;
        }

        // Candidates are enumerated by ascending ordinal, which is the insertion (and therefore
        // enumeration) order the string dominator sets used to produce.
        var candidates = new List<int>();
        for (var i = 0; i < n; i++)
        {
            candidates.Clear();
            var best = -1;
            for (var w = 0; w < words; w++)
            {
                var word = dominators.Word(i, w);
                if (w == i / 64)
                {
                    word &= ~(1UL << (i % 64));
                }

                while (word != 0)
                {
                    var candidate = (w * 64) + BitOperations.TrailingZeroCount(word);
                    word &= word - 1;
                    candidates.Add(candidate);
                    if (best < 0 || domSize[candidate] > domSize[best])
                    {
                        best = candidate;
                    }
                }
            }

            if (candidates.Count == 0)
            {
                idom[blocks[i]] = null;
                continue;
            }

            var chosen = IsImmediateDominator(dominators, i, candidates, best)
                ? best
                : ResolveImmediateDominatorExhaustive(dominators, candidates);
            idom[blocks[i]] = chosen < 0 ? null : blocks[chosen];
        }

        return idom;
    }

    /// <summary>
    /// Confirms that <paramref name="best"/> is the unique candidate that dominates no other
    /// candidate, which is the property the exhaustive search selects on.
    /// </summary>
    private static bool IsImmediateDominator(DominatorBits dominators, int block, List<int> candidates, int best)
    {
        // Dom(best) must equal the strict dominator set of the block. That proves every other
        // candidate dominates best, so no other candidate can qualify.
        for (var w = 0; w < dominators.Words; w++)
        {
            var strict = dominators.Word(block, w);
            if (w == block / 64)
            {
                strict &= ~(1UL << (block % 64));
            }

            if (strict != dominators.Word(best, w))
            {
                return false;
            }
        }

        // ...and best itself must dominate none of them.
        foreach (var candidate in candidates)
        {
            if (candidate != best && dominators.Dominates(best, candidate))
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// Exact O(k^2) fallback for degenerate dominance relations (for example a cycle that is
    /// unreachable from the entry keeps an all-ones dominator set, so its strict dominators are
    /// not totally ordered). Returns the first candidate that dominates no other candidate.
    /// </summary>
    private static int ResolveImmediateDominatorExhaustive(DominatorBits dominators, List<int> candidates)
    {
        foreach (var candidate in candidates)
        {
            var isImmediate = true;
            foreach (var other in candidates)
            {
                if (other == candidate)
                {
                    continue;
                }

                // If candidate dominates other, then candidate is not immediate.
                if (dominators.Dominates(candidate, other))
                {
                    isImmediate = false;
                    break;
                }
            }

            if (isImmediate)
            {
                return candidate;
            }
        }

        return -1;
    }

    private static Dictionary<string, HashSet<string>> ComputeDominanceFrontier(IrCfg cfg, DominatorBits dominators, Dictionary<string, string?> idom)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        var df = cfg.Blocks.Keys.ToDictionary(label => label, _ => new HashSet<string>(comparer), comparer);
        var blocks = dominators.Blocks;
        var n = blocks.Count;

        // Ordinal views of the walk inputs so the runner loop is pure integer work.
        var idomIndex = new int[n];
        var frontiers = new HashSet<string>[n];
        for (var i = 0; i < n; i++)
        {
            var immediate = idom.TryGetValue(blocks[i], out var value) ? value : null;
            idomIndex[i] = immediate == null ? -1 : dominators.Index[immediate];
            frontiers[i] = df[blocks[i]];
        }

        for (var i = 0; i < n; i++)
        {
            var block = blocks[i];
            var preds = cfg.Predecessors(block);
            if (preds.Count < 2)
            {
                continue;
            }

            for (var p = 0; p < preds.Count; p++)
            {
                var runner = dominators.Index[preds[p]];
                while (!dominators.Dominates(runner, i) || runner == i)
                {
                    frontiers[runner].Add(block);
                    var immediateRunner = idomIndex[runner];
                    if (immediateRunner < 0 || immediateRunner == runner)
                    {
                        break;
                    }

                    runner = immediateRunner;
                }
            }
        }

        return df;
    }

    private static IrFunction InsertPhiNodes(IrFunction function, IrCfg cfg, Dictionary<string, HashSet<string>> dominanceFrontier)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        // Map variable name -> blocks containing definitions.
        var defSites = new Dictionary<string, HashSet<string>>(comparer);
        foreach (var block in function.Blocks)
        {
            foreach (var def in DefinitionsIn(block))
            {
                if (IsBlockLocalTemp(def))
                {
                    continue;
                }

                if (!defSites.TryGetValue(def, out var set))
                {
                    set = new HashSet<string>(comparer);
                    defSites[def] = set;
                }
                set.Add(block.Label);
            }
        }

        var newBlocks = function.Blocks.ToDictionary(b => b.Label, b => b.Instructions.ToList(), comparer);
        // Phis are always inserted at index 0; collecting them per block and prepending once
        // avoids the O(n) memmove every Insert(0, ...) would cost.
        var pendingPhis = new Dictionary<string, List<IrPhi>>(comparer);

        foreach (var (variable, defBlocks) in defSites)
        {
            var worklist = new Queue<string>(defBlocks);
            // The variable is fixed for this loop, so only the block has to be tracked.
            var hasAlready = new HashSet<string>(StringComparer.Ordinal);

            while (worklist.Count > 0)
            {
                var x = worklist.Dequeue();
                if (!dominanceFrontier.TryGetValue(x, out var dfSet))
                {
                    continue;
                }
                foreach (var y in dfSet)
                {
                    if (!hasAlready.Add(y))
                    {
                        continue;
                    }

                    // Insert phi at start of block.
                    if (!pendingPhis.TryGetValue(y, out var phis))
                    {
                        phis = new List<IrPhi>();
                        pendingPhis[y] = phis;
                    }
                    phis.Add(new IrPhi(variable, new Dictionary<string, string>()));

                    if (!defBlocks.Contains(y))
                    {
                        worklist.Enqueue(y);
                    }
                }
            }
        }

        foreach (var (label, phis) in pendingPhis)
        {
            // Reversed: each phi was conceptually pushed in front of the previously inserted ones.
            var body = newBlocks[label];
            var merged = new List<IrInstruction>(phis.Count + body.Count);
            for (var i = phis.Count - 1; i >= 0; i--)
            {
                merged.Add(phis[i]);
            }
            merged.AddRange(body);
            newBlocks[label] = merged;
        }

        var rebuilt = newBlocks.Select(kv => new IrBasicBlock(kv.Key, kv.Value)).ToList();
        return new IrFunction(function.Name, function.EntryLabel, rebuilt);
    }

    private static IEnumerable<string> DefinitionsIn(IrBasicBlock block)
    {
        foreach (var ins in block.Instructions)
        {
            switch (ins)
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
                case IrCall call when !string.IsNullOrWhiteSpace(call.Destination):
                    yield return Base(call.Destination);
                    break;
                case IrIndirectCall icall when !string.IsNullOrWhiteSpace(icall.Destination):
                    yield return Base(icall.Destination);
                    break;
                case IrPhi phi:
                    yield return Base(phi.Destination);
                    break;
            }
        }
    }

    private static bool IsBlockLocalTemp(string name) =>
        name.StartsWith("addr_", StringComparison.OrdinalIgnoreCase) &&
        name.EndsWith("_loc", StringComparison.OrdinalIgnoreCase);

    private static IrFunction RenameToSsa(IrFunction withPhis, IrCfg cfg, Dictionary<string, string?> idom)
    {
        // Precompute dominated children using idom
        var comparer = StringComparer.OrdinalIgnoreCase;
        var domChildren = cfg.Blocks.Keys.ToDictionary(k => k, _ => new List<string>(), comparer);
        foreach (var (block, immediateD) in idom)
        {
            if (immediateD != null)
            {
                domChildren[immediateD].Add(block);
            }
        }

        // Work on mutable instruction lists so phi source updates are reflected in the final output.
        var workingBlocks = withPhis.Blocks.ToDictionary(b => b.Label, b => b.Instructions.ToList(), comparer);

        // Renaming replaces instructions in place only, so phi positions stay stable; caching them
        // keeps the successor update from rescanning entire blocks per CFG edge.
        var phiPositions = new Dictionary<string, int[]>(comparer);
        foreach (var (label, instructions) in workingBlocks)
        {
            List<int>? positions = null;
            for (var i = 0; i < instructions.Count; i++)
            {
                if (instructions[i] is IrPhi)
                {
                    (positions ??= new List<int>()).Add(i);
                }
            }

            if (positions != null)
            {
                phiPositions[label] = positions.ToArray();
            }
        }

        var stacks = new Dictionary<string, Stack<string>>(comparer);
        var nextVersions = new Dictionary<string, int>(comparer);
        var renamedBlocks = new Dictionary<string, List<IrInstruction>>(comparer);
        var activeRenameBlocks = new HashSet<string>(comparer);

        // Explicit work stack instead of recursion, since large functions' dominator trees can
        // overflow the CLR stack. A frame with a non-null pop list is the post-order half of a visit.
        var work = new Stack<(string Label, List<(string variable, string version)>? Pops)>();
        work.Push((cfg.Entry, null));

        while (work.Count > 0)
        {
            var (label, pops) = work.Pop();
            if (pops != null)
            {
                // Pop definitions after children are processed.
                for (var i = pops.Count - 1; i >= 0; i--)
                {
                    stacks[pops[i].variable].Pop();
                }

                activeRenameBlocks.Remove(label);
                continue;
            }

            if (!activeRenameBlocks.Add(label))
            {
                throw new InvalidOperationException($"Cycle in dominator tree while renaming block {label}.");
            }

            var instructions = workingBlocks[label];
            var definedHere = new List<(string variable, string version)>();

            // Rename phi definitions first.
            if (phiPositions.TryGetValue(label, out var phiIndices))
            {
                foreach (var i in phiIndices)
                {
                    if (instructions[i] is not IrPhi phi)
                    {
                        continue;
                    }

                    var newName = NewVersion(phi.Destination, stacks, nextVersions);
                    definedHere.Add((Base(phi.Destination), newName));
                    instructions[i] = new IrPhi(newName, new Dictionary<string, string>(phi.Sources));
                }
            }

            for (var i = 0; i < instructions.Count; i++)
            {
                var ins = instructions[i];
                instructions[i] = RenameInstruction(ins, stacks, nextVersions, definedHere);
            }

            renamedBlocks[label] = instructions;

            // Update successor phi inputs using the current version at end of block.
            foreach (var succ in cfg.Successors(label))
            {
                if (!workingBlocks.TryGetValue(succ, out var succList))
                {
                    continue;
                }

                if (!phiPositions.TryGetValue(succ, out var succPhiIndices))
                {
                    continue;
                }

                foreach (var i in succPhiIndices)
                {
                    if (succList[i] is not IrPhi phi)
                    {
                        continue;
                    }

                    var baseVar = Base(phi.Destination);
                    var current = CurrentVersion(baseVar, stacks, nextVersions);
                    var sources = new Dictionary<string, string>(phi.Sources)
                    {
                        [label] = current
                    };
                    succList[i] = new IrPhi(phi.Destination, sources);
                }
            }

            work.Push((label, definedHere));

            // Use precomputed dominated children.
            var children = domChildren[label];
            for (var i = children.Count - 1; i >= 0; i--)
            {
                work.Push((children[i], null));
            }
        }

        // Some blocks may be unreachable due to conservative CFG pruning; keep their original
        // instruction lists so consumers don't fail lookups.
        foreach (var block in withPhis.Blocks)
        {
            if (!renamedBlocks.ContainsKey(block.Label))
            {
                renamedBlocks[block.Label] = block.Instructions.ToList();
            }
        }

        // Preserve the original block order so later structuring keeps fallthrough semantics.
        var rebuilt = withPhis.Blocks
            .Select(b => new IrBasicBlock(b.Label, renamedBlocks[b.Label]))
            .ToList();
        return new IrFunction(withPhis.Name, withPhis.EntryLabel, rebuilt);
    }

    private static IrInstruction RenameInstruction(IrInstruction ins, Dictionary<string, Stack<string>> stacks,
        Dictionary<string, int> nextVersions, List<(string variable, string version)> definedHere)
    {
        switch (ins)
        {
            case IrAssign assign:
                return assign with
                {
                    Value = RenameValue(assign.Value, stacks, nextVersions),
                    Destination = Register(NewVersion(assign.Destination, stacks, nextVersions), definedHere)
                };

            case IrBinary bin:
                return bin with
                {
                    Left = RenameValue(bin.Left, stacks, nextVersions),
                    Right = RenameValue(bin.Right, stacks, nextVersions),
                    Destination = Register(NewVersion(bin.Destination, stacks, nextVersions), definedHere)
                };

            case IrLoad load:
                return load with
                {
                    Address = RenameAddress(load.Address, stacks, nextVersions),
                    Destination = Register(NewVersion(load.Destination, stacks, nextVersions), definedHere)
                };

            case IrStore store:
                return store with
                {
                    Address = RenameAddress(store.Address, stacks, nextVersions),
                    Source = RenameValue(store.Source, stacks, nextVersions)
                };

            case IrCall call:
                var args = call.Arguments.Select(a => RenameValue(a, stacks, nextVersions)).ToList();
                var dest = string.IsNullOrWhiteSpace(call.Destination)
                    ? string.Empty
                    : Register(NewVersion(call.Destination, stacks, nextVersions), definedHere);
                return call with { Destination = dest, Arguments = args };

            case IrIndirectCall icall:
                // Important: rename the target before allocating a new version for the destination.
                // When destination and target refer to the same register (e.g., LR for blrl),
                // renaming in the opposite order would clobber the call target with the freshly
                // assigned version, leading to null/garbage indirect calls in generated code.
                var icallTarget = RenameValue(icall.Target, stacks, nextVersions);
                var icallArgs = icall.Arguments.Select(a => RenameValue(a, stacks, nextVersions)).ToList();
                var icallDest = string.IsNullOrWhiteSpace(icall.Destination)
                    ? string.Empty
                    : Register(NewVersion(icall.Destination, stacks, nextVersions), definedHere);
                return icall with { Destination = icallDest, Arguments = icallArgs, Target = icallTarget };

            case IrSetCrField setCr:
                return setCr with
                {
                    Left = RenameValue(setCr.Left, stacks, nextVersions),
                    Right = RenameValue(setCr.Right, stacks, nextVersions)
                };

            case IrBranch branch:
                return IrRegisterDataFlow.IsRegisterName(branch.ConditionRegister)
                    ? branch with { ConditionRegister = CurrentVersion(Base(branch.ConditionRegister), stacks, nextVersions) }
                    : branch;

            case IrIndirectJump ijump:
                return ijump with { Target = RenameValue(ijump.Target, stacks, nextVersions) };

            case IrJumpTable table:
                return table with { Selector = CurrentVersion(Base(table.Selector), stacks, nextVersions) };

            case IrJump or IrReturn or IrComment or IrTracePpc or IrUndefined:
                return ins switch
                {
                    IrReturn r when r.Value != null => r with { Value = RenameValue(r.Value, stacks, nextVersions) },
                    _ => ins
                };

            case IrPhi phi:
                return phi; // already renamed at definition time; sources filled separately.

            default:
                return ins;
        }
    }

    private static string Register(string name, List<(string variable, string version)> definedHere)
    {
        definedHere.Add((Base(name), name));
        return name;
    }

    private static IrValue RenameValue(IrValue val, Dictionary<string, Stack<string>> stacks,
        Dictionary<string, int> nextVersions) =>
        val.Kind == "register" && val.RegisterName != null
            ? val with { RegisterName = CurrentVersion(Base(val.RegisterName), stacks, nextVersions) }
            : val;

    private static IrAddress RenameAddress(IrAddress address, Dictionary<string, Stack<string>> stacks,
        Dictionary<string, int> nextVersions)
    {
        var renamedBase = CurrentVersion(Base(address.Base), stacks, nextVersions);
        return new IrAddress(renamedBase, address.Offset);
    }

    private static string NewVersion(string original, Dictionary<string, Stack<string>> stacks,
        Dictionary<string, int> nextVersions)
    {
        var baseName = Base(original);
        if (!stacks.TryGetValue(baseName, out var stack))
        {
            stack = new Stack<string>();
            stacks[baseName] = stack;
        }

        var nextId = nextVersions.GetValueOrDefault(baseName);
        nextVersions[baseName] = nextId + 1;
        var version = $"{baseName}_{nextId}";
        stack.Push(version);
        return version;
    }

    private static string CurrentVersion(string baseName, Dictionary<string, Stack<string>> stacks,
        Dictionary<string, int> nextVersions)
    {
        if (!stacks.TryGetValue(baseName, out var stack) || stack.Count == 0)
        {
            // If a use appears before any definition, treat it as an implicit parameter.
            return NewVersion(baseName, stacks, nextVersions);
        }

        return stack.Peek();
    }

    private static string Base(string name) => RegisterNameUtils.StripNumericSuffix(name);

}

public sealed class IrCfg
{
    public Dictionary<string, IrBasicBlock> Blocks { get; }
    public string Entry { get; }
    public Dictionary<string, List<string>> Succ { get; }
    private readonly Dictionary<string, List<string>> _pred;

    public IrCfg(Dictionary<string, IrBasicBlock> blocks, string entry, Dictionary<string, List<string>> succ)
    {
        Blocks = blocks;
        Entry = entry;
        Succ = succ;
        // Precompute predecessors once to avoid O(n) per query
        var comparer = StringComparer.OrdinalIgnoreCase;
        _pred = blocks.Keys.ToDictionary(k => k, _ => new List<string>(), comparer);
        foreach (var (from, targets) in succ)
        {
            foreach (var to in targets)
            {
                if (_pred.TryGetValue(to, out var list))
                {
                    list.Add(from);
                }
            }
        }
    }

    public IReadOnlyList<string> Successors(string label) => Succ.TryGetValue(label, out var s) ? s : Array.Empty<string>();
    public IReadOnlyList<string> Predecessors(string label) => _pred.TryGetValue(label, out var p) ? p : Array.Empty<string>();

    public static IrCfg Build(IrFunction function)
    {
        // Use case-insensitive lookup so label casing differences (e.g. disassembler vs. lifter)
        // don't break CFG construction for self-edges/back-edges such as "bdnz" loops.
        var comparer = StringComparer.OrdinalIgnoreCase;
        var blocks = function.Blocks.ToDictionary(b => b.Label, b => b, comparer);
        var succ = function.Blocks.ToDictionary(b => b.Label, _ => new List<string>(), comparer);

        for (var i = 0; i < function.Blocks.Count; i++)
        {
            var block = function.Blocks[i];
            if (block.Instructions.Count == 0)
            {
                continue;
            }

            var term = block.Instructions[^1];
            var addedEdge = false;
            switch (term)
            {
                case IrBranch br:
                    if (blocks.TryGetValue(br.TrueLabel, out var trueTarget))
                    {
                        succ[block.Label].Add(trueTarget.Label);
                        addedEdge = true;
                    }
                    if (blocks.TryGetValue(br.FalseLabel, out var falseTarget))
                    {
                        succ[block.Label].Add(falseTarget.Label);
                        addedEdge = true;
                    }
                    break;
                case IrJump j:
                    if (blocks.TryGetValue(j.TargetLabel, out var jumpTarget))
                    {
                        succ[block.Label].Add(jumpTarget.Label);
                        addedEdge = true;
                    }
                    break;
                case IrIndirectJump:
                    // Indirect jumps have dynamic targets; conservatively no static successors.
                    // The terminator check below will still prevent fallthrough.
                    break;
                case IrJumpTable table:
                    foreach (var jt in table.Cases)
                    {
                        if (blocks.TryGetValue(jt.TargetLabel, out var jtTarget))
                        {
                            if (!succ[block.Label].Contains(jtTarget.Label))
                            {
                                succ[block.Label].Add(jtTarget.Label);
                            }
                            addedEdge = true;
                        }
                    }
                    break;
            }

            // Implicit fallthrough when block doesn't end in an explicit control transfer.
            var hasTerminator = term is IrBranch or IrJump or IrReturn or IrIndirectJump or IrJumpTable or IrUndefined;
            if (!hasTerminator && !addedEdge && i + 1 < function.Blocks.Count)
            {
                var next = function.Blocks[i + 1];
                if (!succ[block.Label].Contains(next.Label))
                {
                    succ[block.Label].Add(next.Label);
                }
            }
        }

        return new IrCfg(blocks, function.EntryLabel, succ);
    }
}

public sealed record SsaResult(IrFunction Function, IrCfg Cfg)
{
    /// <summary>
    /// Verifies that every register use has a dominating definition using a simple
    /// forward data-flow analysis across the CFG.
    /// </summary>
    public void ValidateUseDef()
    {
        // Names are interned to dense indices so each block's set is a few ulong words and a
        // fixpoint round is word-wise ANDs instead of hashing a fresh HashSet per block. Walk
        // order and the ABI fallback are unchanged, so the pass/throw outcome is identical.
        var blocks = Function.Blocks;
        var blockCount = blocks.Count;
        var names = new ValueNameTable();

        // Definitions per block, walked in original order (definitions before label insert) so a
        // duplicate block label still surfaces the identical ArgumentException at the same point.
        var blockIndexByLabel = new Dictionary<string, int>(blockCount);
        var defIds = new List<int>();
        var defIdStart = new int[blockCount + 1];
        for (var i = 0; i < blockCount; i++)
        {
            var block = blocks[i];
            defIdStart[i] = defIds.Count;
            foreach (var d in BlockDefs(block))
            {
                defIds.Add(names.Intern(d));
            }

            blockIndexByLabel.Add(block.Label, i);
        }

        defIdStart[blockCount] = defIds.Count;

        // Seed entry block with implicit parameters (uses that occur before any local definition).
        var entryBlock = Function.Blocks.Single(b => b.Label == Function.EntryLabel);
        var entryIndex = blockIndexByLabel[entryBlock.Label];
        // The "not defined by the entry block" filter is pure, so it can be applied once the
        // definition bitsets exist instead of while walking the uses.
        var entryUseIds = new List<int>();
        foreach (var u in BlockUses(entryBlock))
        {
            entryUseIds.Add(names.Intern(u));
        }

        // Every possible in/out set member is now interned (sets only get ABI names or definitions,
        // and TrySet makes later additions no-ops), so the bitset width is fixed from here on.
        var wordCount = (names.Count + 63) >> 6;
        var inBits = new ulong[blockCount * wordCount];
        var outBits = new ulong[blockCount * wordCount];
        var defBits = new ulong[blockCount * wordCount];

        for (var i = 0; i < blockCount; i++)
        {
            var offset = i * wordCount;
            var end = defIdStart[i + 1];
            for (var d = defIdStart[i]; d < end; d++)
            {
                var id = defIds[d];
                defBits[offset + (id >> 6)] |= 1UL << id;
            }

            // Seed every block with ABI params to model live-in registers. Indices [0, seed count)
            // are reserved for them, so this is a constant mask OR.
            SeedAbiRegisters(inBits, offset);
            SeedAbiRegisters(outBits, offset);
        }

        var entryOffset = entryIndex * wordCount;
        foreach (var id in entryUseIds)
        {
            var word = id >> 6;
            var bit = 1UL << id;
            if ((defBits[entryOffset + word] & bit) != 0)
            {
                continue;
            }

            inBits[entryOffset + word] |= bit;
            outBits[entryOffset + word] |= bit;
        }

        // Self-loops are ignored for live-ins, or a loop header could never gain definitions from
        // other predecessors. Computed once since the CFG doesn't change during the fixpoint.
        var blockPreds = new int[blockCount][];
        for (var i = 0; i < blockCount; i++)
        {
            var label = blocks[i].Label;
            var preds = Cfg.Predecessors(label);
            var kept = 0;
            for (var p = 0; p < preds.Count; p++)
            {
                if (preds[p] != label)
                {
                    kept++;
                }
            }

            var resolved = kept == 0 ? Array.Empty<int>() : new int[kept];
            var next = 0;
            for (var p = 0; p < preds.Count; p++)
            {
                var pred = preds[p];
                if (pred != label)
                {
                    resolved[next++] = blockIndexByLabel[pred];
                }
            }

            blockPreds[i] = resolved;
        }

        var scratch = new ulong[wordCount];
        var changed = true;
        while (changed)
        {
            changed = false;
            for (var blockIndex = 0; blockIndex < blockCount; blockIndex++)
            {
                var offset = blockIndex * wordCount;
                var preds = blockPreds[blockIndex];
                var predOut = scratch.AsSpan();
                if (preds.Length == 0)
                {
                    inBits.AsSpan(offset, wordCount).CopyTo(predOut); // entry block keeps implicit params
                }
                else
                {
                    outBits.AsSpan(preds[0] * wordCount, wordCount).CopyTo(predOut);
                    for (var p = 1; p < preds.Length; p++)
                    {
                        var other = outBits.AsSpan(preds[p] * wordCount, wordCount);
                        for (var w = 0; w < wordCount; w++)
                        {
                            predOut[w] &= other[w];
                        }
                    }
                }

                var inSpan = inBits.AsSpan(offset, wordCount);
                var outSpan = outBits.AsSpan(offset, wordCount);
                var defSpan = defBits.AsSpan(offset, wordCount);
                for (var w = 0; w < wordCount; w++)
                {
                    var live = predOut[w];
                    if (inSpan[w] != live)
                    {
                        inSpan[w] = live;
                        changed = true;
                    }

                    var merged = live | defSpan[w];
                    if (outSpan[w] != merged)
                    {
                        outSpan[w] = merged;
                        changed = true;
                    }
                }
            }
        }

        // Treat all ABI-visible architectural registers (GPRs, FPRs, CR fields, etc.) as potential live-ins.
        var abiParamSet = AbiParamSet;
        var available = new ulong[wordCount];
        ulong[]? predUnion = null;

        for (var blockIndex = 0; blockIndex < blockCount; blockIndex++)
        {
            var block = blocks[blockIndex];
            inBits.AsSpan(blockIndex * wordCount, wordCount).CopyTo(available);
            // Predecessor out-set union depends only on the block, so build it once lazily (phi-free
            // blocks never pay for it) instead of once per phi.
            var predUnionBuilt = false;
            foreach (var ins in block.Instructions)
            {
                if (ins is IrPhi phi)
                {
                    // The buffer itself is allocated on first use so phi-free functions never pay for it.
                    var union = predUnion ??= new ulong[wordCount];
                    if (!predUnionBuilt)
                    {
                        Array.Clear(union);
                        foreach (var p in Cfg.Predecessors(block.Label))
                        {
                            var predOut = outBits.AsSpan(blockIndexByLabel[p] * wordCount, wordCount);
                            for (var w = 0; w < wordCount; w++)
                            {
                                union[w] |= predOut[w];
                            }
                        }

                        predUnionBuilt = true;
                    }

                    foreach (var src in phi.Sources.Values)
                    {
                        if (names.Contains(union, src))
                        {
                            continue;
                        }

                        var baseName = BaseName(src);
                        if (abiParamSet.Contains(baseName))
                        {
                            names.TrySet(union, src);
                            continue;
                        }

                        throw new InvalidOperationException($"Use of {src} in phi at block {block.Label} has no incoming definition.");
                    }

                    names.TrySet(available, phi.Destination);
                    continue;
                }

                foreach (var use in IrRegisterDataFlow.Uses(ins))
                {
                    if (!names.Contains(available, use))
                    {
                        var baseName = BaseName(use);
                        if (abiParamSet.Contains(baseName))
                        {
                            // Treat as an additional live-in when dominance proof is inconclusive.
                            names.TrySet(available, use);
                            continue;
                        }
                        throw new InvalidOperationException($"Use of {use} in block {block.Label} has no dominating definition.");
                    }
                }

                foreach (var def in IrRegisterDataFlow.Definitions(ins))
                {
                    names.TrySet(available, def);
                }
            }
        }
    }

    private static void SeedAbiRegisters(ulong[] bits, int offset)
    {
        // The seed occupies the lowest indices, and every function's universe contains them, so the
        // mask is never wider than a block's bitset.
        var mask = AbiSeedMask;
        for (var i = 0; i < mask.Length; i++)
        {
            bits[offset + i] |= mask[i];
        }
    }

    /// <summary>
    /// Dense interning of the value names <see cref="ValidateUseDef"/> reasons about, ordinal to
    /// match the original <c>HashSet&lt;string&gt;</c> comparer. ABI seed names get indices
    /// <c>[0, AbiSeedNames.Length)</c> so seeding a block is a constant mask OR.
    /// </summary>
    private sealed class ValueNameTable
    {
        private readonly Dictionary<string, int> _indices = new(AbiSeedIndices);
        private int _count = AbiSeedNames.Length;
        // HashSet<string> tolerates a null element while a dictionary key may not be null, so a null
        // name (only reachable from malformed IR) gets its own reserved slot instead of throwing
        // where the original code did not.
        private int _nullIndex = -1;

        public int Count => _count;

        public int Intern(string name)
        {
            if (name is null)
            {
                if (_nullIndex < 0)
                {
                    _nullIndex = _count++;
                }

                return _nullIndex;
            }

            if (!_indices.TryGetValue(name, out var index))
            {
                index = _count++;
                _indices.Add(name, index);
            }

            return index;
        }

        public bool Contains(ulong[] bits, string name)
            => TryGetIndex(name, out var index) && (bits[index >> 6] & (1UL << index)) != 0;

        /// <summary>
        /// Sets <paramref name="name"/>'s bit when it's part of the interned universe. Names outside
        /// it only ever come from the ABI fallback path, which is a pure re-check every time, so
        /// skipping the insert can't change which violation gets reported.
        /// </summary>
        public void TrySet(ulong[] bits, string name)
        {
            if (TryGetIndex(name, out var index))
            {
                bits[index >> 6] |= 1UL << index;
            }
        }

        private bool TryGetIndex(string name, out int index)
        {
            if (name is null)
            {
                index = _nullIndex;
                return _nullIndex >= 0;
            }

            return _indices.TryGetValue(name, out index);
        }
    }

    private static string BaseName(string name) => RegisterNameUtils.HardwareBase(name);

    private static IEnumerable<string> BlockUses(IrBasicBlock block)
    {
        foreach (var ins in block.Instructions)
        {
            foreach (var u in IrRegisterDataFlow.Uses(ins))
            {
                yield return u;
            }
        }
    }

    private static IEnumerable<string> BlockDefs(IrBasicBlock block)
    {
        foreach (var ins in block.Instructions)
        {
            foreach (var d in IrRegisterDataFlow.Definitions(ins))
            {
                yield return d;
            }
        }
    }

    private static string[] BuildAbiRegisterList()
    {
        var regs = new List<string>();
        regs.AddRange(Enumerable.Range(0, 32).Select(i => $"r{i}"));
        regs.AddRange(Enumerable.Range(0, 32).Select(i => $"f{i}"));
        regs.AddRange(Enumerable.Range(0, 8).Select(i => $"cr{i}"));
        regs.AddRange(Enumerable.Range(0, 8).Select(i => $"gqr{i}"));
        regs.AddRange(new[]
        {
            "cr",
            "lr",
            "ctr",
            "xer",
            "fpscr",
            "msr",
            "dar",
            "dsisr",
            "iccr",
            "tbr",
            "tbl",
            "tbu",
            "hid0",
            "hid1",
            "hid2",
            "srr0",
            "srr1"
        });

        return regs
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static readonly string[] AbiRegisters = BuildAbiRegisterList();

    // Immutable seeds shared by every validation run; they are only ever read.
    private static readonly HashSet<string> AbiParamSet = AbiRegisters.ToHashSet(StringComparer.OrdinalIgnoreCase);

    private static readonly HashSet<string> AbiRegistersWithVersions = AbiParamSet
        .SelectMany(p => new[] { p, $"{p}_0" })
        .ToHashSet(StringComparer.OrdinalIgnoreCase);

    // ValidateUseDef seeds every block's in/out set with exactly these names. They are the same for
    // every function, so they get the lowest interned indices: the per-block seed becomes a constant
    // mask OR, and the per-call interning table is a bulk copy of a prebuilt dictionary.
    private static readonly string[] AbiSeedNames = AbiRegistersWithVersions.ToArray();

    // Ordinal (default) comparer on purpose: membership in the seeded HashSet<string> sets is ordinal
    // even though the set was *built* from a case-insensitive one, so "R3" must not resolve to "r3".
    private static readonly Dictionary<string, int> AbiSeedIndices = BuildAbiSeedIndices();

    private static readonly ulong[] AbiSeedMask = BuildAbiSeedMask();

    private static Dictionary<string, int> BuildAbiSeedIndices()
    {
        var map = new Dictionary<string, int>(AbiSeedNames.Length);
        for (var i = 0; i < AbiSeedNames.Length; i++)
        {
            map[AbiSeedNames[i]] = i;
        }

        return map;
    }

    private static ulong[] BuildAbiSeedMask()
    {
        var mask = new ulong[(AbiSeedNames.Length + 63) >> 6];
        for (var i = 0; i < AbiSeedNames.Length; i++)
        {
            mask[i >> 6] |= 1UL << i;
        }

        return mask;
    }
}
