using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private static IrFunction ElideDeadPureLocalTemps(IrFunction function)
    {
        var cfg = IrCfg.Build(function);
        var liveIn = function.Blocks.ToDictionary(
            block => block.Label,
            _ => new HashSet<string>(StringComparer.OrdinalIgnoreCase),
            StringComparer.OrdinalIgnoreCase);
        var liveOut = function.Blocks.ToDictionary(
            block => block.Label,
            _ => new HashSet<string>(StringComparer.OrdinalIgnoreCase),
            StringComparer.OrdinalIgnoreCase);

        var changed = true;
        while (changed)
        {
            changed = false;
            for (var blockIndex = function.Blocks.Count - 1; blockIndex >= 0; blockIndex--)
            {
                var block = function.Blocks[blockIndex];
                var nextOut = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (var successor in cfg.Successors(block.Label))
                {
                    if (liveIn.TryGetValue(successor, out var successorIn))
                    {
                        nextOut.UnionWith(successorIn);
                    }
                }

                var nextIn = new HashSet<string>(nextOut, StringComparer.OrdinalIgnoreCase);
                for (var insIndex = block.Instructions.Count - 1; insIndex >= 0; insIndex--)
                {
                    UpdateLivePureLocalTemps(block.Instructions[insIndex], nextIn);
                }

                if (!nextOut.SetEquals(liveOut[block.Label]))
                {
                    liveOut[block.Label] = nextOut;
                    changed = true;
                }

                if (!nextIn.SetEquals(liveIn[block.Label]))
                {
                    liveIn[block.Label] = nextIn;
                    changed = true;
                }
            }
        }

        var rewrittenBlocks = new IrBasicBlock[function.Blocks.Count];
        var removedAny = false;

        for (var blockIndex = function.Blocks.Count - 1; blockIndex >= 0; blockIndex--)
        {
            var block = function.Blocks[blockIndex];
            var rewritten = new List<IrInstruction>(block.Instructions.Count);
            var live = new HashSet<string>(liveOut[block.Label], StringComparer.OrdinalIgnoreCase);

            for (var insIndex = block.Instructions.Count - 1; insIndex >= 0; insIndex--)
            {
                var instruction = block.Instructions[insIndex];
                if (instruction is IrAssign assign && IsDeadPureLocalDestination(assign.Destination, live))
                {
                    removedAny = true;
                    continue;
                }

                if (instruction is IrBinary binary && IsDeadPureLocalDestination(binary.Destination, live))
                {
                    removedAny = true;
                    continue;
                }

                rewritten.Add(instruction);
                UpdateLivePureLocalTemps(instruction, live);
            }

            rewritten.Reverse();
            rewrittenBlocks[blockIndex] = removedAny && rewritten.Count != block.Instructions.Count
                ? block with { Instructions = rewritten }
                : block;
        }

        return removedAny
            ? function with { Blocks = rewrittenBlocks }
            : function;
    }

    private static bool IsDeadPureLocalDestination(string destination, HashSet<string> live)
    {
        return !IsCpuRegister(destination) && !live.Contains(destination);
    }

    private static void UpdateLivePureLocalTemps(IrInstruction instruction, HashSet<string> live)
    {
        switch (instruction)
        {
            case IrAssign assign:
                KillIfPureLocal(assign.Destination, live);
                AddIfPureLocal(assign.Value, live);
                break;

            case IrBinary binary:
                KillIfPureLocal(binary.Destination, live);
                AddIfPureLocal(binary.Left, live);
                AddIfPureLocal(binary.Right, live);
                break;

            case IrLoad load:
                // Preserve all memory reads. They can validate guest addresses or expose
                // unsupported hardware reads even when the loaded value is dead.
                KillIfPureLocal(load.Destination, live);
                AddIfPureLocal(load.Address.Base, live);
                break;

            case IrStore store:
                AddIfPureLocal(store.Address.Base, live);
                AddIfPureLocal(store.Source, live);
                break;

            case IrCall call:
                if (!string.IsNullOrWhiteSpace(call.Destination))
                {
                    KillIfPureLocal(call.Destination, live);
                }
                foreach (var arg in call.Arguments)
                {
                    AddIfPureLocal(arg, live);
                }
                break;

            case IrIndirectCall indirectCall:
                if (!string.IsNullOrWhiteSpace(indirectCall.Destination))
                {
                    KillIfPureLocal(indirectCall.Destination, live);
                }
                AddIfPureLocal(indirectCall.Target, live);
                foreach (var arg in indirectCall.Arguments)
                {
                    AddIfPureLocal(arg, live);
                }
                break;

            case IrSetCrField setCr:
                AddIfPureLocal(setCr.Left, live);
                AddIfPureLocal(setCr.Right, live);
                break;

            case IrPhi phi:
                if (!IsCpuRegister(phi.Destination))
                {
                    live.Remove(phi.Destination);
                }
                foreach (var source in phi.Sources.Values)
                {
                    AddIfPureLocal(source, live);
                }
                break;

            case IrBranch branch when IsCpuRegister(branch.ConditionRegister):
                AddIfPureLocal(branch.ConditionRegister, live);
                break;

            case IrIndirectJump indirectJump:
                AddIfPureLocal(indirectJump.Target, live);
                break;

            case IrJumpTable jumpTable:
                AddIfPureLocal(jumpTable.Selector, live);
                break;

            case IrReturn { Value: { } value }:
                AddIfPureLocal(value, live);
                break;
        }
    }

    private static void AddIfPureLocal(IrValue value, HashSet<string> live)
    {
        if (value.Kind == "register" && value.RegisterName != null)
        {
            AddIfPureLocal(value.RegisterName, live);
        }
    }

    private static void AddIfPureLocal(string name, HashSet<string> live)
    {
        if (!IsCpuRegister(name))
        {
            live.Add(name);
        }
    }

    private static void KillIfPureLocal(string name, HashSet<string> live)
    {
        if (!IsCpuRegister(name))
        {
            live.Remove(name);
        }
    }
}
