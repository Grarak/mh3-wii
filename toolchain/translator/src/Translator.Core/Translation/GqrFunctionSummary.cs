using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

/// <summary>
/// Compact projection of a function's SSA IR, keeping only what can affect interprocedural GQR
/// propagation (unlike the full <see cref="IrFunction"/>, which is discarded between discovery and emission).
/// </summary>
public sealed class GqrFunctionSummary
{
    private const int GqrCount = 8;

    private GqrFunctionSummary(
        int entryBlock,
        GqrSummaryBlock[] blocks,
        uint[] directGuestTargets,
        sbyte[] gqrAffinityByVariable,
        byte localWriteMask,
        bool hasUnknownGuestOrIndirectCall)
    {
        EntryBlock = entryBlock;
        Blocks = blocks;
        DirectGuestTargets = directGuestTargets;
        GqrAffinityByVariable = gqrAffinityByVariable;
        LocalWriteMask = localWriteMask;
        HasUnknownGuestOrIndirectCall = hasUnknownGuestOrIndirectCall;
    }

    internal int EntryBlock { get; }
    internal IReadOnlyList<GqrSummaryBlock> Blocks { get; }
    public IReadOnlyList<uint> DirectGuestTargets { get; }
    private sbyte[] GqrAffinityByVariable { get; }
    internal byte LocalWriteMask { get; }
    internal bool HasUnknownGuestOrIndirectCall { get; }

    public static GqrFunctionSummary Create(IrFunction function)
    {
        ArgumentNullException.ThrowIfNull(function);

        var blockIndices = function.Blocks
            .Select((block, index) => (block.Label, index))
            .ToDictionary(pair => pair.Label, pair => pair.index, StringComparer.OrdinalIgnoreCase);
        var cfg = IrCfg.Build(function);
        var variableIds = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var gqrAffinityByVariable = Enumerable.Range(0, GqrCount)
            .Select(index => checked((sbyte)index))
            .ToList();
        var nextVariableId = GqrCount;
        var directGuestTargets = new HashSet<uint>();
        byte localWriteMask = 0;
        var hasUnknownGuestOrIndirectCall = false;

        int ExactVariable(string name)
        {
            if (name.Length == 4 && name.StartsWith("gqr", StringComparison.OrdinalIgnoreCase) &&
                name[3] is >= '0' and <= '7')
            {
                return name[3] - '0';
            }

            if (!variableIds.TryGetValue(name, out var id))
            {
                id = nextVariableId++;
                variableIds.Add(name, id);
                var gqrKey = GqrConstantPropagation.TryGetGqrKey(name);
                gqrAffinityByVariable.Add(gqrKey is null ? (sbyte)-1 : checked((sbyte)(gqrKey[3] - '0')));
            }
            return id;
        }

        int AssignDestination(string name)
        {
            var key = GqrConstantPropagation.TryGetGqrKey(name);
            return key is null ? ExactVariable(name) : key[3] - '0';
        }

        GqrSummaryValue Value(IrValue value)
        {
            if (value is { Kind: "const", Constant: { } immediate })
                return GqrSummaryValue.ConstantValue(unchecked((uint)immediate));
            if (value is { Kind: "register", RegisterName: { } register })
                return GqrSummaryValue.VariableValue(ExactVariable(register));
            return GqrSummaryValue.Unknown;
        }

        var blocks = new GqrSummaryBlock[function.Blocks.Count];
        for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
        {
            var block = function.Blocks[blockIndex];
            var instructions = new List<GqrSummaryInstruction>();
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrAssign assign:
                    {
                        var destination = AssignDestination(assign.Destination);
                        if (destination < GqrCount)
                            localWriteMask |= (byte)(1 << destination);
                        instructions.Add(GqrSummaryInstruction.Assign(destination, Value(assign.Value)));
                        break;
                    }
                    case IrBinary binary:
                        instructions.Add(GqrSummaryInstruction.Binary(
                            ExactVariable(binary.Destination),
                            Value(binary.Left),
                            Value(binary.Right),
                            ParseBinary(binary.Op)));
                        break;
                    case IrPhi phi:
                        instructions.Add(GqrSummaryInstruction.Phi(
                            ExactVariable(phi.Destination),
                            phi.Sources.Select(source => new GqrPhiSource(
                                blockIndices.TryGetValue(source.Key, out var predecessor) ? predecessor : -1,
                                ExactVariable(source.Value))).ToArray()));
                        break;
                    case IrLoad load:
                        instructions.Add(GqrSummaryInstruction.Kill(ExactVariable(load.Destination)));
                        break;
                    case IrCall call when IsGuestCall(call.Target):
                    {
                        var targetKnown = GqrConstantPropagation.TryGuestAddress(call.Target, out var target);
                        if (targetKnown)
                            directGuestTargets.Add(target);
                        else
                            hasUnknownGuestOrIndirectCall = true;
                        instructions.Add(GqrSummaryInstruction.GuestCall(target, targetKnown));
                        break;
                    }
                    case IrCall call when !string.IsNullOrWhiteSpace(call.Destination):
                        instructions.Add(GqrSummaryInstruction.Kill(ExactVariable(call.Destination)));
                        break;
                    case IrIndirectCall:
                        hasUnknownGuestOrIndirectCall = true;
                        instructions.Add(GqrSummaryInstruction.IndirectCall());
                        break;
                }
            }

            var predecessors = cfg.Predecessors(block.Label)
                .Select(label => blockIndices.TryGetValue(label, out var predecessor) ? predecessor : -1)
                .Where(predecessor => predecessor >= 0)
                .ToArray();
            blocks[blockIndex] = new GqrSummaryBlock(predecessors, instructions.ToArray());
        }

        return new GqrFunctionSummary(
            blockIndices.TryGetValue(function.EntryLabel, out var entryBlock) ? entryBlock : 0,
            blocks,
            directGuestTargets.OrderBy(target => target).ToArray(),
            gqrAffinityByVariable.ToArray(),
            localWriteMask,
            hasUnknownGuestOrIndirectCall);
    }

    internal int GetGqrAffinity(int variable) =>
        variable >= 0 && variable < GqrAffinityByVariable.Length
            ? GqrAffinityByVariable[variable]
            : -1;

    private static bool IsGuestCall(string target) =>
        target.StartsWith("func_", StringComparison.OrdinalIgnoreCase) ||
        target.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ||
        uint.TryParse(target, out _);

    private static GqrBinaryOperation ParseBinary(string op) => op.ToLowerInvariant() switch
    {
        "add" or "add_u" => GqrBinaryOperation.Add,
        "sub" or "sub_u" => GqrBinaryOperation.Subtract,
        "or" => GqrBinaryOperation.Or,
        "and" => GqrBinaryOperation.And,
        "xor" => GqrBinaryOperation.Xor,
        "shl" => GqrBinaryOperation.ShiftLeft,
        "rotl" => GqrBinaryOperation.RotateLeft,
        "shr" or "shr_u" => GqrBinaryOperation.ShiftRight,
        _ => GqrBinaryOperation.Unknown
    };
}

internal sealed record GqrSummaryBlock(
    int[] Predecessors,
    GqrSummaryInstruction[] Instructions);

internal enum GqrSummaryOperation : byte
{
    Assign,
    Binary,
    Phi,
    Kill,
    GuestCall,
    IndirectCall
}

internal enum GqrBinaryOperation : byte
{
    Unknown,
    Add,
    Subtract,
    Or,
    And,
    Xor,
    ShiftLeft,
    RotateLeft,
    ShiftRight
}

internal readonly record struct GqrSummaryValue(byte Kind, int Variable, uint Constant)
{
    public static GqrSummaryValue Unknown => default;
    public static GqrSummaryValue VariableValue(int variable) => new(1, variable, 0);
    public static GqrSummaryValue ConstantValue(uint constant) => new(2, 0, constant);
}

internal readonly record struct GqrPhiSource(int Predecessor, int Variable);

internal readonly record struct GqrSummaryInstruction(
    GqrSummaryOperation Operation,
    int Destination,
    GqrSummaryValue Left,
    GqrSummaryValue Right,
    GqrBinaryOperation BinaryOperation,
    uint CallTarget,
    bool CallTargetKnown,
    GqrPhiSource[]? PhiSources)
{
    public static GqrSummaryInstruction Assign(int destination, GqrSummaryValue value) =>
        new(GqrSummaryOperation.Assign, destination, value, default, default, 0, false, null);
    public static GqrSummaryInstruction Binary(
        int destination, GqrSummaryValue left, GqrSummaryValue right, GqrBinaryOperation operation) =>
        new(GqrSummaryOperation.Binary, destination, left, right, operation, 0, false, null);
    public static GqrSummaryInstruction Phi(int destination, GqrPhiSource[] sources) =>
        new(GqrSummaryOperation.Phi, destination, default, default, default, 0, false, sources);
    public static GqrSummaryInstruction Kill(int destination) =>
        new(GqrSummaryOperation.Kill, destination, default, default, default, 0, false, null);
    public static GqrSummaryInstruction GuestCall(uint target, bool targetKnown) =>
        new(GqrSummaryOperation.GuestCall, 0, default, default, default, target, targetKnown, null);
    public static GqrSummaryInstruction IndirectCall() =>
        new(GqrSummaryOperation.IndirectCall, 0, default, default, default, 0, false, null);
}
