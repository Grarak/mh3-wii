using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private readonly record struct StackAccessInfo(int Offset, int SizeBytes, bool IsStore);

    private static IrFunction ElideFctiwStackLowWordLoads(IrFunction func, RepresentationEnvironment types)
    {
        var stackFacts = StackAddressFacts.Build(func);
        var cfg = IrCfg.Build(func);
        var changed = false;
        var nextTemp = 0;
        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);

        foreach (var block in func.Blocks)
        {
            var insertedBefore = new Dictionary<int, List<IrInstruction>>();
            var replacements = new Dictionary<int, IrInstruction>();
            var removals = new HashSet<int>();
            var claimedLoads = new HashSet<int>();
            var convertedFprs = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);

            for (var index = 0; index < block.Instructions.Count; index++)
            {
                var instruction = block.Instructions[index];
                if (instruction is IrStore store &&
                    store.SizeBytes == 8 &&
                    store.Source.Kind == "register" &&
                    store.Source.RegisterName is { } sourceRegister &&
                    IsFloatRegister(sourceRegister) &&
                    convertedFprs.TryGetValue(GetRegisterBaseName(sourceRegister), out var converted) &&
                    converted &&
                    stackFacts.TryResolve(store.Address, out var storeOffset) &&
                    TryFindFctiwLowWordLoad(block.Instructions, index, stackFacts, storeOffset, claimedLoads, out var loadIndex))
                {
                    var temp = $"fctiwzword{nextTemp++}";
                    AddInsertion(index, new IrBinary(temp, IrValue.Register(sourceRegister), IrValue.Imm(0), "fpr_low_word"));
                    replacements[loadIndex] = new IrAssign(((IrLoad)block.Instructions[loadIndex]).Destination, IrValue.Register(temp));
                    if (CanRemoveFctiwStackStore(func, cfg, block.Label, loadIndex + 1, stackFacts, storeOffset))
                    {
                        removals.Add(index);
                    }
                    claimedLoads.Add(loadIndex);
                    types.Unify(temp, ValueRepresentation.UInt32);
                    changed = true;
                }

                UpdateConvertedFprState(instruction, convertedFprs);
            }

            if (insertedBefore.Count == 0 && replacements.Count == 0 && removals.Count == 0)
            {
                rewrittenBlocks.Add(block);
                continue;
            }

            var rewrittenInstructions = new List<IrInstruction>(block.Instructions.Count + insertedBefore.Count);
            for (var index = 0; index < block.Instructions.Count; index++)
            {
                if (insertedBefore.TryGetValue(index, out var inserted))
                {
                    rewrittenInstructions.AddRange(inserted);
                }

                if (removals.Contains(index))
                {
                    continue;
                }

                rewrittenInstructions.Add(replacements.TryGetValue(index, out var replacement)
                    ? replacement
                    : block.Instructions[index]);
            }

            rewrittenBlocks.Add(new IrBasicBlock(block.Label, rewrittenInstructions));

            void AddInsertion(int index, IrInstruction inserted)
            {
                if (!insertedBefore.TryGetValue(index, out var list))
                {
                    list = new List<IrInstruction>();
                    insertedBefore[index] = list;
                }

                list.Add(inserted);
            }
        }

        return changed
            ? new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks)
            : func;
    }

    private static bool CanRemoveFctiwStackStore(
        IrFunction func,
        IrCfg cfg,
        string startBlockLabel,
        int startIndex,
        StackAddressFacts stackFacts,
        int storeOffset)
    {
        var pending = new Queue<(string BlockLabel, int StartIndex)>();
        var visited = new HashSet<(string BlockLabel, int StartIndex)>();
        pending.Enqueue((startBlockLabel, startIndex));

        while (pending.Count > 0)
        {
            var (blockLabel, blockStartIndex) = pending.Dequeue();
            if (!visited.Add((blockLabel, blockStartIndex)))
            {
                continue;
            }

            if (!cfg.Blocks.TryGetValue(blockLabel, out var block))
            {
                continue;
            }

            var overwrittenOnThisPath = false;
            for (var index = Math.Max(0, blockStartIndex); index < block.Instructions.Count; index++)
            {
                var instruction = block.Instructions[index];
                if (IsGuestMemoryBarrier(instruction))
                {
                    return false;
                }

                if (!TryGetStackAccessInfo(instruction, stackFacts, out var access) ||
                    !RangesOverlap(storeOffset, 8, access.Offset, access.SizeBytes))
                {
                    continue;
                }

                if (!access.IsStore)
                {
                    return false;
                }

                overwrittenOnThisPath = true;
                break;
            }

            if (overwrittenOnThisPath)
            {
                continue;
            }

            foreach (var successor in cfg.Successors(blockLabel))
            {
                pending.Enqueue((successor, 0));
            }
        }

        return true;
    }

    private static bool TryFindFctiwLowWordLoad(
        IReadOnlyList<IrInstruction> instructions,
        int storeIndex,
        StackAddressFacts stackFacts,
        int storeOffset,
        IReadOnlySet<int> claimedLoads,
        out int loadIndex)
    {
        for (var index = storeIndex + 1; index < instructions.Count; index++)
        {
            if (!TryGetStackAccessInfo(instructions[index], stackFacts, out var access) ||
                !RangesOverlap(storeOffset, 8, access.Offset, access.SizeBytes))
            {
                continue;
            }

            if (!access.IsStore &&
                access.SizeBytes == 4 &&
                access.Offset == storeOffset + 4 &&
                !claimedLoads.Contains(index) &&
                instructions[index] is IrLoad)
            {
                loadIndex = index;
                return true;
            }

            break;
        }

        loadIndex = -1;
        return false;
    }

    private static bool TryGetStackAccessInfo(
        IrInstruction instruction,
        StackAddressFacts stackFacts,
        out StackAccessInfo access)
    {
        switch (instruction)
        {
            case IrStore store when stackFacts.TryResolve(store.Address, out var storeOffset):
                access = new StackAccessInfo(storeOffset, store.SizeBytes, true);
                return true;

            case IrLoad load when stackFacts.TryResolve(load.Address, out var loadOffset):
                access = new StackAccessInfo(loadOffset, load.SizeBytes, false);
                return true;

            case IrCall call when TryGetPsqStackAccess(call, stackFacts, out var psq):
                access = new StackAccessInfo(psq.Offset, psq.SizeBytes, psq.IsStore);
                return true;

            default:
                access = default;
                return false;
        }
    }

    private static void UpdateConvertedFprState(
        IrInstruction instruction,
        Dictionary<string, bool> convertedFprs)
    {
        switch (instruction)
        {
            case IrBinary binary when IsFloatRegister(binary.Destination):
                convertedFprs[GetRegisterBaseName(binary.Destination)] = IsFctiwOp(binary.Op);
                break;

            case IrAssign assign when IsFloatRegister(assign.Destination):
                convertedFprs[GetRegisterBaseName(assign.Destination)] = false;
                break;

            case IrLoad load when IsFloatRegister(load.Destination):
                convertedFprs[GetRegisterBaseName(load.Destination)] = false;
                break;

            case IrCall call:
                if (!string.IsNullOrWhiteSpace(call.Destination) &&
                    IsFloatRegister(call.Destination))
                {
                    convertedFprs[GetRegisterBaseName(call.Destination)] = false;
                }
                else if (string.IsNullOrWhiteSpace(call.Destination) &&
                         IsGuestCallTarget(call.Target))
                {
                    convertedFprs.Clear();
                }
                break;

            case IrIndirectCall:
                convertedFprs.Clear();
                break;
        }
    }

    private static bool IsFctiwOp(string op) =>
        op.Equals("fctiw", StringComparison.OrdinalIgnoreCase) ||
        op.Equals("fctiwz", StringComparison.OrdinalIgnoreCase);

    private static bool IsGuestMemoryBarrier(IrInstruction instruction) =>
        instruction is IrIndirectCall ||
        instruction is IrCall call && IsGuestCallTarget(call.Target);
}
