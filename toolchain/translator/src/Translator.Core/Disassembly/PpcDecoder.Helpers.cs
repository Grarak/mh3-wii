using System.Buffers.Binary;

namespace Translator.Core.Disassembly;

public static partial class PpcDecoder
{
    private static int GetField(uint word, int startBit, int width = 5)
    {
        var mask = (1 << width) - 1;
        return (int)((word >> startBit) & (uint)mask);
    }

    private static int SignExtend(uint value, int bits)
    {
        var shift = 32 - bits;
        return (int)(value << shift) >> shift;
    }

    private static string? TryDecodeCrBranchMnemonic(int bo, int bi)
    {
        // Mask off the hint bits: BO=001zy (4/5) branches when the CR bit is false, BO=011zy (12/13) when true.
        var baseBo = bo & 0x1E;
        if (baseBo is not (4 or 12))
        {
            return null;
        }

        var crBit = bi % 4;
        var table = baseBo == 12 ? TrueCrBranchMnemonics : FalseCrBranchMnemonics;

        return table[crBit];
    }

    private static readonly string[] TrueCrBranchMnemonics = { "blt", "bgt", "beq", "bso" };

    private static readonly string[] FalseCrBranchMnemonics = { "bge", "ble", "bne", "bns" };

    // Operands are immutable records compared by value, so caching one shared instance per
    // register number avoids an allocation on every decoded operand without changing behavior.
    private const int RegisterFileSize = 32;

    private static readonly string[] GprNames = BuildRegisterNames("r");

    private static readonly string[] FprNames = BuildRegisterNames("f");

    private static readonly PpcRegisterOperand[] GprOperands = BuildRegisterOperands(GprNames);

    private static readonly PpcRegisterOperand[] FprOperands = BuildRegisterOperands(FprNames);

    private static readonly PpcConditionRegisterOperand[] CrFieldOperands = BuildCrFieldOperands();

    private static readonly PpcConditionRegisterOperand[] CrBitOperands = BuildCrBitOperands();

    private static string[] BuildRegisterNames(string prefix)
    {
        var names = new string[RegisterFileSize];
        for (var i = 0; i < names.Length; i++)
        {
            names[i] = prefix + i.ToString(System.Globalization.CultureInfo.InvariantCulture);
        }

        return names;
    }

    private static PpcRegisterOperand[] BuildRegisterOperands(string[] names)
    {
        var operands = new PpcRegisterOperand[names.Length];
        for (var i = 0; i < operands.Length; i++)
        {
            operands[i] = new PpcRegisterOperand(names[i], i);
        }

        return operands;
    }

    private static PpcConditionRegisterOperand[] BuildCrFieldOperands()
    {
        var operands = new PpcConditionRegisterOperand[8];
        for (var i = 0; i < operands.Length; i++)
        {
            operands[i] = new PpcConditionRegisterOperand($"cr{i}", i * 4);
        }

        return operands;
    }

    private static PpcConditionRegisterOperand[] BuildCrBitOperands()
    {
        var operands = new PpcConditionRegisterOperand[RegisterFileSize];
        for (var i = 0; i < operands.Length; i++)
        {
            operands[i] = new PpcConditionRegisterOperand($"crb{i}", i);
        }

        return operands;
    }

    private static PpcRegisterOperand Reg(int number) =>
        (uint)number < (uint)GprOperands.Length
            ? GprOperands[number]
            : new PpcRegisterOperand($"r{number}", number);

    private static string RegName(int number) =>
        (uint)number < (uint)GprNames.Length ? GprNames[number] : $"r{number}";

    private static PpcRegisterOperand FReg(int number) =>
        (uint)number < (uint)FprOperands.Length
            ? FprOperands[number]
            : new PpcRegisterOperand($"f{number}", number);

    private static PpcConditionRegisterOperand CrField(int field) =>
        (uint)field < (uint)CrFieldOperands.Length
            ? CrFieldOperands[field]
            : new PpcConditionRegisterOperand($"cr{field}", field * 4);

    private static PpcConditionRegisterOperand CrBit(int bit) =>
        (uint)bit < (uint)CrBitOperands.Length
            ? CrBitOperands[bit]
            : new PpcConditionRegisterOperand($"crb{bit}", bit);

    private static int DecodeSpr(uint word)
    {
        // SPR is split across bits 11-15 (high) and 16-20 (low)
        return ((int)(word >> 16) & 0x1F) | (((int)word >> 6) & 0x3E0);
    }
}
