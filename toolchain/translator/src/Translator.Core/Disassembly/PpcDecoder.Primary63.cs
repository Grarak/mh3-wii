using System.Buffers.Binary;

namespace Translator.Core.Disassembly;

public static partial class PpcDecoder
{
    private static void DecodePrimary63(uint address, uint word, List<PpcOperand> operands, List<uint> branches,
        ref string mnemonic, ref bool isReturn, ref bool isCall, ref bool isCond)
    {
        var xo = GetField(word, 1, 10);
        var rc = (word & 1) != 0;
        var frt = GetField(word, 21);
        var fra = GetField(word, 16);
        var frb = GetField(word, 11);
        var frc = GetField(word, 6);
        var xo5 = xo & 0x1F;

        switch (xo5)
        {
            case 23: // fsel
                mnemonic = rc ? "fsel." : "fsel";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                return;
            case 25: // fmul
                mnemonic = rc ? "fmul." : "fmul";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                return;
            case 28: // fmsub
                mnemonic = rc ? "fmsub." : "fmsub";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                return;
            case 29: // fmadd
                mnemonic = rc ? "fmadd." : "fmadd";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                return;
            case 30: // fnmsub
                mnemonic = rc ? "fnmsub." : "fnmsub";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                return;
            case 31: // fnmadd
                mnemonic = rc ? "fnmadd." : "fnmadd";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frc));
                operands.Add(FReg(frb));
                return;
        }

        switch (xo)
        {
            case 64: // mcrfs
            {
                var crfD = GetField(word, 23, 3);
                var crfS = GetField(word, 18, 3);
                mnemonic = "mcrfs";
                operands.Add(CrField(crfD));
                operands.Add(CrField(crfS));
                isCond = true;
                break;
            }

            case 0: // fcmpu
            {
                var crfD = GetField(word, 23, 3);
                mnemonic = "fcmpu";
                operands.Add(CrField(crfD));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                isCond = true;
                break;
            }

            case 32: // fcmpo
            {
                var crfD = GetField(word, 23, 3);
                mnemonic = "fcmpo";
                operands.Add(CrField(crfD));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                isCond = true;
                break;
            }

            case 38: // mtfsb1
                mnemonic = rc ? "mtfsb1." : "mtfsb1";
                operands.Add(new PpcImmediateOperand(frt));
                break;

            case 70: // mtfsb0
                mnemonic = rc ? "mtfsb0." : "mtfsb0";
                operands.Add(new PpcImmediateOperand(frt));
                break;

            case 134: // mtfsfi
                mnemonic = rc ? "mtfsfi." : "mtfsfi";
                operands.Add(new PpcImmediateOperand(GetField(word, 23, 3)));
                operands.Add(new PpcImmediateOperand(GetField(word, 12, 4)));
                break;

            case 583: // mffs
                mnemonic = rc ? "mffs." : "mffs";
                operands.Add(FReg(frt));
                break;

            case 711: // mtfsf
                mnemonic = rc ? "mtfsf." : "mtfsf";
                operands.Add(new PpcImmediateOperand((int)((word >> 17) & 0xFF)));
                operands.Add(FReg(frb));
                break;

            case 14: // fctiw
                mnemonic = rc ? "fctiw." : "fctiw";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 15: // fctiwz
                mnemonic = rc ? "fctiwz." : "fctiwz";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 12: // frsp
                mnemonic = rc ? "frsp." : "frsp";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 18: // fdiv
                mnemonic = rc ? "fdiv." : "fdiv";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;

            case 20: // fsub
                mnemonic = rc ? "fsub." : "fsub";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;

            case 21: // fadd
                mnemonic = rc ? "fadd." : "fadd";
                operands.Add(FReg(frt));
                operands.Add(FReg(fra));
                operands.Add(FReg(frb));
                break;

            case 24: // fres
                mnemonic = rc ? "fres." : "fres";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 26: // frsqrte
                mnemonic = rc ? "frsqrte." : "frsqrte";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 40: // fneg
                mnemonic = rc ? "fneg." : "fneg";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 136: // fnabs
                mnemonic = rc ? "fnabs." : "fnabs";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 72: // fmr
                mnemonic = rc ? "fmr." : "fmr";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            case 264: // fabs
                mnemonic = rc ? "fabs." : "fabs";
                operands.Add(FReg(frt));
                operands.Add(FReg(frb));
                break;

            default:
                mnemonic = $"fp_{xo}";
                break;
        }
    }
}
