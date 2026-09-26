namespace Translator.Core.Disassembly;

public static partial class PpcDecoder
{
    private static void DecodePrimary59(uint address, uint word, List<PpcOperand> operands, List<uint> branches,
        ref string mnemonic, ref bool isReturn, ref bool isCall, ref bool isCond)
    {
        var xo = GetField(word, 1, 5);
        var rc = (word & 1) != 0;
        var frt = GetField(word, 21);
        var fra = GetField(word, 16);
        var frb = GetField(word, 11);
        var frc = GetField(word, 6);

        switch (xo)
        {
            case 18:
                mnemonic = rc ? "fdivs." : "fdivs";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;
            case 20:
                mnemonic = rc ? "fsubs." : "fsubs";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;
            case 21:
                mnemonic = rc ? "fadds." : "fadds";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;
            case 24:
                mnemonic = rc ? "fres." : "fres";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;
            case 25:
                mnemonic = rc ? "fmuls." : "fmuls";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                break;
            case 28:
                mnemonic = rc ? "fmsubs." : "fmsubs";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                break;
            case 29:
                mnemonic = rc ? "fmadds." : "fmadds";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                break;
            case 30:
                mnemonic = rc ? "fnmsubs." : "fnmsubs";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                break;
            case 31:
                mnemonic = rc ? "fnmadds." : "fnmadds";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                break;
            default:
                mnemonic = "opc_59";
                break;
        }
    }
}
