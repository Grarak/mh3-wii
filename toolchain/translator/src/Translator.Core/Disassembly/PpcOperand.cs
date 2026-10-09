using System;

namespace Translator.Core.Disassembly;

/// <summary>
/// Typed representation of a PowerPC instruction operand to avoid any string parsing in later stages.
/// </summary>
public abstract record PpcOperand
{
    public abstract string ToOperandString();

    public override string ToString() => ToOperandString();
}

public sealed record PpcRegisterOperand(string Name, int Number) : PpcOperand
{
    public override string ToOperandString() => Name;
}

public sealed record PpcImmediateOperand(int Value, bool PreferHex = false) : PpcOperand
{
    public override string ToOperandString()
    {
        if (!PreferHex)
        {
            return Value.ToString();
        }

        return Value < 0 ? $"-0x{Math.Abs(Value):X}" : $"0x{Value:X}";
    }
}

public sealed record PpcDisplacementOperand(int Offset, string BaseRegister, int BaseRegisterNumber) : PpcOperand
{
    public override string ToOperandString()
    {
        var text = Offset == 0 ? "0" : (Offset < 0 ? $"-0x{Math.Abs(Offset):X}" : $"0x{Offset:X}");
        return $"{text}({BaseRegister})";
    }
}

public sealed record PpcBranchTargetOperand(uint TargetAddress) : PpcOperand
{
    public override string ToOperandString() => $"0x{TargetAddress:X8}";
}

public sealed record PpcConditionRegisterOperand(string Name, int BitIndex) : PpcOperand
{
    public override string ToOperandString() => Name;
}
