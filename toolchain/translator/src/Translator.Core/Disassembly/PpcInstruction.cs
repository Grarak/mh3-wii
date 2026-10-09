using System;
using System.Collections.Generic;
using System.Linq;

namespace Translator.Core.Disassembly;

/// <summary>
/// Light wrapper over a decoded PowerPC instruction with semantic flags and typed operands.
/// This type is intentionally immutable so tests can compare instances by value.
/// </summary>
public sealed class PpcInstruction
{
    public PpcInstruction(
        uint address,
        uint word,
        string mnemonic,
        IReadOnlyList<PpcOperand> operands,
        IReadOnlyList<uint> branchTargets,
        bool isReturn,
        bool isCall,
        bool isConditionalBranch)
    {
        Address = address;
        Word = word;
        Mnemonic = mnemonic;
        Operands = operands;
        BranchTargets = branchTargets;
        IsReturn = isReturn;
        IsCall = isCall;
        IsConditionalBranch = isConditionalBranch;
    }

    public uint Address { get; }

    /// <summary>
    /// The raw big-endian instruction word, replacing the former byte array to save two
    /// allocations per decoded instruction.
    /// </summary>
    public uint Word { get; }

    public string Mnemonic { get; }
    public IReadOnlyList<PpcOperand> Operands { get; }
    public IReadOnlyList<uint> BranchTargets { get; }
    public bool IsReturn { get; }
    public bool IsCall { get; }
    public bool IsConditionalBranch { get; }

    public string OperandText => string.Join(", ", Operands.Select(o => o.ToOperandString()));

    public uint EndAddress => checked(Address + 4u);

    public bool IsUnconditionalBranch => BranchTargets.Count > 0 && !IsConditionalBranch && !IsCall && !IsReturn;

    public override string ToString() => $"0x{Address:X8}: {Mnemonic} {OperandText}".Trim();

    public static PpcInstruction Synthetic(
        uint address,
        uint opcode,
        string mnemonic,
        IReadOnlyList<PpcOperand> operands,
        IReadOnlyList<uint>? branchTargets = null,
        bool? isReturn = null,
        bool? isCall = null,
        bool? isConditional = null)
    {
        var (ret, call, cond) = InferFlags(mnemonic, isReturn, isCall, isConditional);
        return new PpcInstruction(address, opcode, mnemonic, operands, branchTargets ?? Array.Empty<uint>(), ret, call, cond);
    }

    private static (bool ret, bool call, bool cond) InferFlags(string mnemonic, bool? retOpt, bool? callOpt, bool? condOpt)
    {
        if (retOpt.HasValue || callOpt.HasValue || condOpt.HasValue)
        {
            return (retOpt ?? false, callOpt ?? false, condOpt ?? false);
        }

        var lower = mnemonic.ToLowerInvariant();
        var ret = lower is "bctr" or "rfi" ||
              (lower.StartsWith("b", StringComparison.Ordinal) && lower.EndsWith("lr", StringComparison.Ordinal) &&
               !lower.Contains("lrl", StringComparison.Ordinal));
        var call = lower is "bl" or "bcl" or "blrl" || lower.Contains("bctrl") || lower.Contains("bclrl");
        var cond = lower.StartsWith("bc") || lower.StartsWith("bge") || lower.StartsWith("ble") || lower.StartsWith("beq") ||
                   lower.StartsWith("bne") || lower.StartsWith("bgt") || lower.StartsWith("blt") || lower.StartsWith("bd");
        return (ret, call, cond);
    }
}
