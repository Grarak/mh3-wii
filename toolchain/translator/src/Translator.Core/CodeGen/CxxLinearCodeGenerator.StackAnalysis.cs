using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>Identifies one IR instruction by its position in the function.</summary>
    private readonly record struct InstructionKey(string BlockLabel, int Index);

    /// <summary>
    /// An <see cref="InstructionKey"/> plus a whole-function ordinal, so two
    /// sites in different blocks can be compared in emission order.
    /// </summary>
    private readonly record struct InstructionSite(string BlockLabel, int Index, int Order)
    {
        public InstructionKey Key => new(BlockLabel, Index);
    }

    private sealed record StackAccess(InstructionSite Site, int Offset, int SizeBytes);

    private sealed class FprStackSaveSlot
    {
        public List<InstructionSite> DirectStores { get; } = new();
        public List<InstructionSite> DirectLoads { get; } = new();
        public List<InstructionSite> PsqStores { get; } = new();
        public List<InstructionSite> PsqLoads { get; } = new();
    }

    /// <summary>Replaces a leaf function's private ABI FPR stack spill with a native local. Requires
    /// one entry save, one epilogue restore, one return, no overlapping stack access, and, for paired
    /// saves, canonical scalar+PSQ save / PSQ+scalar restore order.</summary>
    private static IrFunction ElideLeafStackFprSaveRestore(
        IrFunction func,
        RepresentationEnvironment types)
    {
        if (!FunctionEligibleForLeafRegisterCache(func))
            return func;

        var stackFacts = StackAddressFacts.Build(func);
        var stackAccesses = new List<StackAccess>();
        var slots = new Dictionary<(int Fpr, int Offset), FprStackSaveSlot>();
        var psqAddressTemps = new Dictionary<InstructionKey, string>();
        var returnSites = new List<InstructionSite>();
        var orderedInstructions = new List<(InstructionSite Site, IrInstruction Instruction)>();

        var order = 0;
        foreach (var block in func.Blocks)
        {
            for (var index = 0; index < block.Instructions.Count; ++index, ++order)
            {
                var instruction = block.Instructions[index];
                var site = new InstructionSite(block.Label, index, order);
                orderedInstructions.Add((site, instruction));
                switch (instruction)
                {
                    case IrReturn:
                        returnSites.Add(site);
                        break;
                    case IrStore store:
                        RecordDirectStackStore(store, site);
                        break;
                    case IrLoad load:
                        RecordDirectStackLoad(load, site);
                        break;
                    case IrCall call:
                        RecordPsqStackAccess(call, site);
                        break;
                }
            }
        }

        if (returnSites.Count != 1)
            return func;

        var replacements = new Dictionary<InstructionKey, IrInstruction>();
        var removals = new HashSet<InstructionKey>();
        var savedFprs = new SortedSet<int>();
        var stackTempUseCounts = CountRegisterUses(func);
        var stackTempDefinitions = CollectStackAddressTemporaryDefinitions(func, stackFacts);
        var returnSite = returnSites[0];

        foreach (var ((fpr, offset), slot) in slots.OrderBy(pair => pair.Key.Fpr).ThenBy(pair => pair.Key.Offset))
        {
            if (slot.DirectStores.Count != 1 || slot.DirectLoads.Count != 1)
                continue;

            var directStore = slot.DirectStores[0];
            var directLoad = slot.DirectLoads[0];
            if (!directStore.BlockLabel.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase) ||
                !directLoad.BlockLabel.Equals(returnSite.BlockLabel, StringComparison.OrdinalIgnoreCase) ||
                directStore.Order >= directLoad.Order || directLoad.Order >= returnSite.Order ||
                HasFprDefinitionBefore(fpr, directStore.Order))
            {
                continue;
            }

            var slotSites = new HashSet<InstructionKey> { directStore.Key, directLoad.Key };
            var accessSize = 8;
            var hasPairedSave = slot.PsqStores.Count == 1 && slot.PsqLoads.Count == 1;
            if (hasPairedSave)
            {
                var psqStore = slot.PsqStores[0];
                var psqLoad = slot.PsqLoads[0];
                if (!psqStore.BlockLabel.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase) ||
                    !psqLoad.BlockLabel.Equals(returnSite.BlockLabel, StringComparison.OrdinalIgnoreCase) ||
                    !(directStore.Order < psqStore.Order && psqStore.Order < psqLoad.Order &&
                      psqLoad.Order < directLoad.Order) ||
                    HasFprAccessBetween(fpr, psqLoad.Order, directLoad.Order))
                {
                    continue;
                }

                slotSites.Add(psqStore.Key);
                slotSites.Add(psqLoad.Key);
                accessSize = 16;
            }
            else if (slot.PsqStores.Count != 0 || slot.PsqLoads.Count != 0)
            {
                continue;
            }

            if (HasOverlappingStackAccess(stackAccesses, slotSites, offset, accessSize))
                continue;

            savedFprs.Add(fpr);
            removals.Add(directStore.Key);
            replacements[directLoad.Key] = new IrAssign($"f{fpr}", IrValue.Register(LeafStackSavedFprName(fpr)));

            if (hasPairedSave)
            {
                var psqStore = slot.PsqStores[0];
                var psqLoad = slot.PsqLoads[0];
                removals.Add(psqStore.Key);
                removals.Add(psqLoad.Key);
                RemoveSingleUsePsqAddressTemporary(psqStore.Key);
                RemoveSingleUsePsqAddressTemporary(psqLoad.Key);
            }
        }

        if (savedFprs.Count == 0)
            return func;

        foreach (var fpr in savedFprs)
            types.Unify(LeafStackSavedFprName(fpr), ValueRepresentation.Float64);

        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);
        foreach (var block in func.Blocks)
        {
            var rewritten = new List<IrInstruction>(block.Instructions.Count + savedFprs.Count);
            if (block.Label.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase))
            {
                foreach (var fpr in savedFprs)
                    // This pass runs after SSA construction. Preserve the
                    // explicit live-in version so later LLVM lowering never
                    // mistakes the epilogue's synthetic fN definition for the
                    // entry value being saved.
                    rewritten.Add(new IrAssign(LeafStackSavedFprName(fpr), IrValue.Register($"f{fpr}_0")));
            }

            for (var index = 0; index < block.Instructions.Count; ++index)
            {
                var key = new InstructionKey(block.Label, index);
                if (removals.Contains(key))
                    continue;
                rewritten.Add(replacements.TryGetValue(key, out var replacement)
                    ? replacement
                    : block.Instructions[index]);
            }
            rewrittenBlocks.Add(new IrBasicBlock(block.Label, rewritten));
        }

        return new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks);

        bool HasFprDefinitionBefore(int fpr, int limit)
        {
            var name = $"f{fpr}";
            return orderedInstructions.Any(item => item.Site.Order < limit &&
                InstructionDefinesRegister(item.Instruction, name));
        }

        bool HasFprAccessBetween(int fpr, int after, int before)
        {
            var name = $"f{fpr}";
            return orderedInstructions.Any(item => item.Site.Order > after && item.Site.Order < before &&
                (InstructionUsesRegister(item.Instruction, name) || InstructionDefinesRegister(item.Instruction, name)));
        }

        void RemoveSingleUsePsqAddressTemporary(InstructionKey psqKey)
        {
            if (psqAddressTemps.TryGetValue(psqKey, out var name) &&
                stackTempUseCounts.TryGetValue(name, out var useCount) && useCount == 1 &&
                stackTempDefinitions.TryGetValue(name, out var definition))
            {
                removals.Add(definition.Key);
            }
        }

        FprStackSaveSlot GetSlot(int fpr, int offset)
        {
            var key = (fpr, offset);
            if (!slots.TryGetValue(key, out var slot))
                slots[key] = slot = new FprStackSaveSlot();
            return slot;
        }

        void RecordDirectStackStore(IrStore store, InstructionSite site)
        {
            if (!stackFacts.TryResolve(store.Address, out var offset))
                return;
            stackAccesses.Add(new StackAccess(site, offset, store.SizeBytes));
            if (store.SizeBytes == 8 && store.Source.Kind == "register" &&
                TryParseNonvolatileFpr(store.Source.RegisterName, out var fpr))
            {
                GetSlot(fpr, offset).DirectStores.Add(site);
            }
        }

        void RecordDirectStackLoad(IrLoad load, InstructionSite site)
        {
            if (!stackFacts.TryResolve(load.Address, out var offset))
                return;
            stackAccesses.Add(new StackAccess(site, offset, load.SizeBytes));
            if (load.SizeBytes == 8 && TryParseNonvolatileFpr(load.Destination, out var fpr))
                GetSlot(fpr, offset).DirectLoads.Add(site);
        }

        void RecordPsqStackAccess(IrCall call, InstructionSite site)
        {
            if (!TryGetPsqStackAccess(call, stackFacts, out var psq))
                return;
            stackAccesses.Add(new StackAccess(site, psq.Offset, psq.SizeBytes));
            if (!psq.IsSaveRestoreCandidate || psq.SizeBytes != 8 || psq.Offset < 8)
                return;
            var slot = GetSlot(psq.Fpr, psq.Offset - 8);
            if (call.Arguments.Count > 0 && call.Arguments[0].Kind == "register" &&
                call.Arguments[0].RegisterName is { } temporary)
            {
                psqAddressTemps[site.Key] = temporary;
            }
            if (psq.IsStore) slot.PsqStores.Add(site); else slot.PsqLoads.Add(site);
        }
    }

    private static string LeafStackSavedFprName(int fpr) => $"leaf_stack_saved_f{fpr}_entry";

    private static IrFunction ElideOverwrittenPsqStackLoads(IrFunction func)
    {
        var stackFacts = StackAddressFacts.Build(func);
        var stackTempUseCounts = CountRegisterUses(func);
        var stackTempDefinitions = CollectStackAddressTemporaryDefinitions(func, stackFacts);
        var removals = new HashSet<InstructionKey>();

        foreach (var block in func.Blocks)
        {
            for (var index = 0; index + 1 < block.Instructions.Count; index++)
            {
                if (block.Instructions[index] is not IrCall call ||
                    !TryGetPsqStackAccess(call, stackFacts, out var psq) ||
                    psq.IsStore ||
                    !psq.IsSaveRestoreCandidate ||
                    psq.SizeBytes != 8 ||
                    psq.Offset < 8 ||
                    !TryFindOverwritingScalarFprLoad(block.Instructions, index, stackFacts, psq, out _))
                {
                    continue;
                }

                var callKey = new InstructionKey(block.Label, index);
                removals.Add(callKey);

                if (call.Arguments.Count > 0 &&
                    call.Arguments[0].Kind == "register" &&
                    call.Arguments[0].RegisterName is { } psqTemp &&
                    stackTempUseCounts.TryGetValue(psqTemp, out var useCount) &&
                    useCount == 1 &&
                    stackTempDefinitions.TryGetValue(psqTemp, out var defSite))
                {
                    removals.Add(defSite.Key);
                }
            }
        }

        if (removals.Count == 0)
        {
            return func;
        }

        var rewrittenBlocks = new List<IrBasicBlock>(func.Blocks.Count);
        foreach (var block in func.Blocks)
        {
            var rewrittenInstructions = new List<IrInstruction>(block.Instructions.Count);
            for (var index = 0; index < block.Instructions.Count; index++)
            {
                if (!removals.Contains(new InstructionKey(block.Label, index)))
                {
                    rewrittenInstructions.Add(block.Instructions[index]);
                }
            }

            rewrittenBlocks.Add(new IrBasicBlock(block.Label, rewrittenInstructions));
        }

        return new IrFunction(func.Name, func.EntryLabel, rewrittenBlocks);
    }

    private static bool TryFindOverwritingScalarFprLoad(
        IReadOnlyList<IrInstruction> instructions,
        int psqLoadIndex,
        StackAddressFacts stackFacts,
        PsqStackAccess psq,
        out IrLoad load)
    {
        for (var index = psqLoadIndex + 1; index < instructions.Count; index++)
        {
            var instruction = instructions[index];
            switch (instruction)
            {
                case IrComment:
                case IrTracePpc:
                    continue;

                case IrLoad candidate
                    when candidate.SizeBytes == 8 &&
                         TryParseNonvolatileFpr(candidate.Destination, out var loadFpr) &&
                         loadFpr == psq.Fpr &&
                         stackFacts.TryResolve(candidate.Address, out var loadOffset) &&
                         loadOffset == psq.Offset - 8:
                    load = candidate;
                    return true;

                case IrCall:
                case IrIndirectCall:
                case IrBranch:
                case IrJump:
                case IrIndirectJump:
                case IrJumpTable:
                case IrReturn:
                    load = default!;
                    return false;

                default:
                    if (InstructionUsesRegister(instruction, $"f{psq.Fpr}") ||
                        InstructionDefinesRegister(instruction, $"f{psq.Fpr}"))
                    {
                        load = default!;
                        return false;
                    }

                    continue;
            }
        }

        load = default!;
        return false;
    }

    private static bool InstructionUsesRegister(IrInstruction instruction, string registerName)
    {
        var baseName = GetRegisterBaseName(registerName);
        return IrRegisterDataFlow.Uses(instruction).Any(name =>
            GetRegisterBaseName(name).Equals(baseName, StringComparison.OrdinalIgnoreCase));
    }

    private static bool InstructionDefinesRegister(IrInstruction instruction, string registerName)
    {
        var baseName = GetRegisterBaseName(registerName);
        return IrRegisterDataFlow.Definitions(instruction).Any(name =>
            GetRegisterBaseName(name).Equals(baseName, StringComparison.OrdinalIgnoreCase));
    }

    private static Dictionary<string, InstructionSite> CollectStackAddressTemporaryDefinitions(
        IrFunction func,
        StackAddressFacts stackFacts)
    {
        var definitions = new Dictionary<string, InstructionSite>(StringComparer.OrdinalIgnoreCase);
        var order = 0;
        foreach (var block in func.Blocks)
        {
            for (var index = 0; index < block.Instructions.Count; index++, order++)
            {
                var instruction = block.Instructions[index];
                string? destination = instruction switch
                {
                    IrAssign assign => assign.Destination,
                    IrBinary binary => binary.Destination,
                    _ => null
                };

                if (destination != null && stackFacts.ContainsTemporary(destination))
                {
                    definitions[destination] = new InstructionSite(block.Label, index, order);
                }
            }
        }

        return definitions;
    }

    private static Dictionary<string, int> CountRegisterUses(IrFunction func)
    {
        var counts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

        foreach (var block in func.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                // Keep every occurrence: stack-temp elision relies on a
                // true use count, not a set of referenced names.
                foreach (var name in IrRegisterDataFlow.Uses(instruction))
                {
                    if (!string.IsNullOrWhiteSpace(name))
                    {
                        counts[name] = counts.TryGetValue(name, out var count) ? count + 1 : 1;
                    }
                }
            }
        }

        return counts;
    }

    private sealed record PsqStackAccess(
        bool IsStore,
        bool IsSaveRestoreCandidate,
        int Fpr,
        int Offset,
        int SizeBytes);

    private static bool TryGetPsqStackAccess(
        IrCall call,
        StackAddressFacts stackFacts,
        out PsqStackAccess access)
    {
        access = default!;
        var isStore = string.Equals(call.Target, "PPC_PsqSt", StringComparison.Ordinal);
        var isLoad = string.Equals(call.Target, "PPC_PsqL", StringComparison.Ordinal);
        if (!isStore && !isLoad)
        {
            return false;
        }

        if (call.Arguments.Count < (isStore ? 4 : 3) ||
            !stackFacts.TryResolve(call.Arguments[0], out var offset))
        {
            return false;
        }

        var widthIndex = isStore ? 2 : 1;
        var quantIndex = isStore ? 3 : 2;
        var width = TryGetIntConstant(call.Arguments[widthIndex], out var widthValue)
            ? widthValue
            : -1;
        var quant = TryGetIntConstant(call.Arguments[quantIndex], out var quantValue)
            ? quantValue
            : -1;
        var sizeBytes = width == 1 ? 4 : 8;
        var candidate = width == 0 && quant == 0;
        var fpr = -1;

        if (candidate)
        {
            if (isStore)
            {
                candidate = call.Arguments[1].Kind == "register" &&
                            TryParseNonvolatileFpr(call.Arguments[1].RegisterName, out fpr);
            }
            else
            {
                candidate = TryParseNonvolatileFpr(call.Destination, out fpr);
            }
        }

        access = new PsqStackAccess(isStore, candidate, fpr, offset, sizeBytes);
        return true;
    }

    private static bool TryGetIntConstant(IrValue value, out int result)
    {
        if (value.Kind == "const" &&
            value.Constant is { } constant &&
            constant >= int.MinValue &&
            constant <= int.MaxValue)
        {
            result = (int)constant;
            return true;
        }

        result = 0;
        return false;
    }

    private static bool TryParseNonvolatileFpr(string? registerName, out int fpr)
    {
        if (!string.IsNullOrWhiteSpace(registerName) &&
            TryParseFloatRegisterIndex(GetRegisterBaseName(registerName), out fpr) &&
            fpr is >= 14 and <= 31)
        {
            return true;
        }

        fpr = -1;
        return false;
    }

    private static bool HasOverlappingStackAccess(
        IReadOnlyList<StackAccess> accesses,
        IReadOnlySet<InstructionKey> allowedSites,
        int offset,
        int sizeBytes)
    {
        foreach (var access in accesses)
        {
            if (allowedSites.Contains(access.Site.Key))
            {
                continue;
            }

            if (RangesOverlap(offset, sizeBytes, access.Offset, access.SizeBytes))
            {
                return true;
            }
        }

        return false;
    }

    private static bool RangesOverlap(int leftOffset, int leftSize, int rightOffset, int rightSize)
    {
        var leftEnd = leftOffset + leftSize;
        var rightEnd = rightOffset + rightSize;
        return leftOffset < rightEnd && rightOffset < leftEnd;
    }
}
