using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;

namespace Translator.Core.Analysis.BasicBlocks;

/// <summary>
/// Slices a linear instruction list into basic blocks and wires up successor/predecessor links.
/// </summary>
public static class BasicBlockBuilder
{
    public static IReadOnlyList<BasicBlock> Build(
        IReadOnlyList<PpcInstruction> instructions,
        IEnumerable<uint>? extraLeaders = null)
    {
        if (instructions.Count == 0)
        {
            return Array.Empty<BasicBlock>();
        }

        var leaders = new HashSet<uint> { instructions[0].Address };
        if (extraLeaders is not null)
        {
            // Only the requested leaders need a containment structure; building
            // one over every instruction address was the expensive half.
            HashSet<uint>? requested = null;
            foreach (var leader in extraLeaders)
            {
                (requested ??= new HashSet<uint>()).Add(leader);
            }

            if (requested is not null)
            {
                foreach (var instruction in instructions)
                {
                    if (requested.Contains(instruction.Address))
                    {
                        leaders.Add(instruction.Address);
                    }
                }
            }
        }

        for (var i = 0; i < instructions.Count; i++)
        {
            var ins = instructions[i];
            if (i > 0)
            {
                var previous = instructions[i - 1];
                if (previous.IsReturn ||
                    previous.IsUnconditionalBranch ||
                    previous.EndAddress != ins.Address)
                {
                    leaders.Add(ins.Address);
                }
            }

            if (ins.IsConditionalBranch)
            {
                foreach (var target in ins.BranchTargets)
                {
                    leaders.Add(target);
                }

                leaders.Add(ins.EndAddress); // fallthrough
            }
            else if (ins.IsUnconditionalBranch)
            {
                foreach (var target in ins.BranchTargets)
                {
                    leaders.Add(target);
                }
            }
        }

        var sortedLeaders = new uint[leaders.Count];
        leaders.CopyTo(sortedLeaders);
        Array.Sort(sortedLeaders);

        // One pass over the instructions instead of a full rescan per leader.
        // Each instruction lands in the block of the greatest leader that is not
        // above it, which is exactly the half-open range the old filter used.
        var buckets = new List<PpcInstruction>?[sortedLeaders.Length];
        foreach (var instruction in instructions)
        {
            var address = instruction.Address;
            var index = Array.BinarySearch(sortedLeaders, address);
            if (index < 0)
            {
                index = ~index - 1;
            }

            if (index < 0)
            {
                continue;
            }

            // The final block's exclusive end was uint.MaxValue.
            if (index == sortedLeaders.Length - 1 && address == uint.MaxValue)
            {
                continue;
            }

            (buckets[index] ??= new List<PpcInstruction>()).Add(instruction);
        }

        var blocks = new List<BasicBlock>();
        for (var i = 0; i < sortedLeaders.Length; i++)
        {
            var blockInstructions = buckets[i];
            if (blockInstructions is null || blockInstructions.Count == 0)
            {
                continue;
            }

            var block = new BasicBlock(sortedLeaders[i], blockInstructions);
            blocks.Add(block);
        }

        // Wire successors/predecessors.
        var lookup = blocks.ToDictionary(b => b.StartAddress, b => b);
        foreach (var block in blocks)
        {
            var term = block.Terminator;
            if (term.IsConditionalBranch)
            {
                foreach (var target in term.BranchTargets)
                {
                    AddEdge(block, lookup, target);
                }

                AddEdge(block, lookup, term.EndAddress);
            }
            else if (term.IsUnconditionalBranch || term.IsCall)
            {
                foreach (var target in term.BranchTargets)
                {
                    AddEdge(block, lookup, target);
                }
            }
            else if (!term.IsReturn)
            {
                // Fallthrough
                AddEdge(block, lookup, term.EndAddress);
            }
        }

        return blocks;
    }

    private static void AddEdge(BasicBlock from, IDictionary<uint, BasicBlock> lookup, uint toAddress)
    {
        if (!lookup.TryGetValue(toAddress, out var to))
        {
            return;
        }

        if (!from.Successors.Contains(toAddress))
        {
            from.Successors.Add(toAddress);
        }

        if (!to.Predecessors.Contains(from.StartAddress))
        {
            to.Predecessors.Add(from.StartAddress);
        }
    }
}
