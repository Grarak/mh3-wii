using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

public sealed partial class PpcLifter
{
    private static IReadOnlyList<IrInstruction>? TryLiftFloating(
        PpcInstruction ins,
        string mnemonic,
        IReadOnlyList<PpcOperand> ops,
        IReadOnlyList<string> operands)
    {
        switch (mnemonic)
        {
            case "mffs":
            case "mffs.":
            case "fp_583":
                return LiftMffs(ins);

            case "mtfsf":
            case "mtfsf.":
            case "fp_711":
                return LiftMtfsf(ins);

            case "mtfsfi":
            case "mtfsfi.":
            case "fp_134":
                return LiftMtfsfi(ins);

            case "mtfsb0":
            case "mtfsb0.":
            case "fp_70":
                return LiftMtfsb0(ins);

            case "mtfsb1":
            case "mtfsb1.":
            case "fp_38":
                return LiftMtfsb1(ins);

            case "fadd" when operands.Count == 3:
            case "fadd." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "add") };

            case "fsub" when operands.Count == 3:
            case "fsub." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "sub") };

            case "fmul" when operands.Count == 3:
            case "fmul." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "mul") };

            case "fdiv" when operands.Count == 3:
            case "fdiv." when operands.Count == 3:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Register(operands[2]), "fdiv") };

            case "fadds" when operands.Count == 3:
            case "fadds." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fsubs" when operands.Count == 3:
            case "fsubs." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fmuls" when operands.Count == 3:
            case "fmuls." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fmuls", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fdivs" when operands.Count == 3:
            case "fdivs." when operands.Count == 3:
                return new[] { new IrCall(operands[0], "PPC_Fdivs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]) }) };

            case "fres" when operands.Count == 2:
            case "fres." when operands.Count == 2:
                return new[] { new IrCall(operands[0], "PPC_Fres", new[] { IrValue.Register(operands[1]) }) };

            case "fmsubs" when operands.Count == 4:
            case "fmsubs." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fmadds" when operands.Count == 4:
            case "fmadds." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmsubs" when operands.Count == 4:
            case "fnmsubs." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmsubs", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmadds" when operands.Count == 4:
            case "fnmadds." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmadds", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fp_57":
                {
                    // fp_57 corresponds to Opcode 63 with bits 21-30 = 57.
                    // This splits into C=1 and XO=25 (fmul).
                    // We decode as A-Form to be safe.
                    uint raw1 = ReadRawInstruction(ins);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);
                    // XO is at bits 26-30 (raw >> 1 & 0x1F). 
                    // We assume fmul based on the ID, but decoding standard A-form logic works too.
                    return new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), "mul") };
                }

            case "frsp":
            case "frsp.":
            case "fp_12":
                {
                    string dest, src;
                    // Handle cases where disassembler fallback (fp_12) might not provide parsed operands
                    if (operands.Count >= 2)
                    {
                        dest = operands[0];
                        src = operands[1];
                    }
                    else
                    {
                        uint raw1 = ReadRawInstruction(ins);
                        int rD = (int)((raw1 >> 21) & 0x1F);
                        int rB = (int)((raw1 >> 11) & 0x1F);
                        dest = $"f{rD}";
                        src = $"f{rB}";
                    }
                    
                    // frsp is a unary operation, mapped here as a binary op with a dummy 0 immediate
                    // similar to how fneg/fabs are often handled in this lifter.
                    return new[] { new IrBinary(dest, IrValue.Register(src), IrValue.Imm(0), "frsp") };
                }
            case "fp_281":
                {
                    // fp_281 is fmul with rC=8. 
                    // 281 = (8 << 5) | 25. XO 25 is fmul.
                    uint raw1 = ReadRawInstruction(ins);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);
                    return new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), "mul") };
                }
            case "fneg" when operands.Count == 2:
            case "fneg." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fneg") };

            case "fabs" when operands.Count == 2:
            case "fabs." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fabs") };

            case "fnabs" when operands.Count == 2:
            case "fnabs." when operands.Count == 2:
                return new[]
                {
                    new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fabs"),
                    new IrBinary(operands[0], IrValue.Register(operands[0]), IrValue.Imm(0), "fneg")
                };

            case "fmr" when operands.Count == 2:
            case "fmr." when operands.Count == 2:
                return new[] { new IrAssign(operands[0], IrValue.Register(operands[1])) };

            case "fctiw" when operands.Count == 2:
            case "fctiw." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fctiw") };

            case "fctiwz" when operands.Count == 2:
            case "fctiwz." when operands.Count == 2:
                return new[] { new IrBinary(operands[0], IrValue.Register(operands[1]), IrValue.Imm(0), "fctiwz") };

            case "fsel" when operands.Count == 4:
            case "fsel." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fsel", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[3]), IrValue.Register(operands[2]) }) };

            case "frsqrte" when operands.Count == 2:
            case "frsqrte." when operands.Count == 2:
                return new[] { new IrCall(operands[0], "PPC_Frsqrte", new[] { IrValue.Register(operands[1]) }) };

            case "fmsub" when operands.Count == 4:
            case "fmsub." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmsub", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fmadd" when operands.Count == 4:
            case "fmadd." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fmadd", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmsub" when operands.Count == 4:
            case "fnmsub." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmsub", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fnmadd" when operands.Count == 4:
            case "fnmadd." when operands.Count == 4:
                return new[] { new IrCall(operands[0], "PPC_Fnmadd", new[] { IrValue.Register(operands[1]), IrValue.Register(operands[2]), IrValue.Register(operands[3]) }) };

            case "fcmpu" when operands.Count == 3:
            case "fcmpo" when operands.Count == 3:
                {
                    // Model the CR write explicitly so leaf/resident register caches update
                    // the authoritative CR storage instead of a stale CpuContext shadow.
                    var destCr = ParseCrFieldIndex(operands[0]);
                    return new[]
                    {
                        new IrSetCrField(destCr, IrValue.Register(operands[1]),
                            IrValue.Register(operands[2]), false)
                    };
                }

            case "opc_59":
                {
                    uint raw1 = ReadRawInstruction(ins);
                    // Decode fields for A-Form instruction
                    // XO is at bits 26-30 (bits 1-5 in our raw uint from LSB)
                    int xo = (int)((raw1 >> 1) & 0x1F);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rB = (int)((raw1 >> 11) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);

                    return xo switch
                    {
                        18 => new[] { new IrCall($"f{rD}", "PPC_Fdivs", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // fdivs
                        20 => new[] { new IrCall($"f{rD}", "PPC_Fsubs", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // fsubs
                        21 => new[] { new IrCall($"f{rD}", "PPC_Fadds", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // fadds
                        22 => new[] { new IrCall($"f{rD}", "PPC_Fsqrt", new[] { IrValue.Register($"f{rB}") }) }, // fsqrts
                        23 => new[] { new IrCall($"f{rD}", "PPC_Fsel", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), IrValue.Register($"f{rC}") }) }, // fsel
                        24 => new[] { new IrCall($"f{rD}", "PPC_Fres", new[] { IrValue.Register($"f{rB}") }) }, // fres
                        25 => new[] { new IrCall($"f{rD}", "PPC_Fmuls", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}") }) }, // fmuls (uses A and C)
                        26 => new[] { new IrCall($"f{rD}", "PPC_Frsqrte", new[] { IrValue.Register($"f{rB}") }) }, // frsqrtes
                        28 => new[] { new IrCall($"f{rD}", "PPC_Fmsubs", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fmsubs
                        29 => new[] { new IrCall($"f{rD}", "PPC_Fmadds", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fmadds
                        30 => new[] { new IrCall($"f{rD}", "PPC_Fnmsubs", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fnmsubs
                        31 => new[] { new IrCall($"f{rD}", "PPC_Fnmadds", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fnmadds
                        _ => throw new NotImplementedException($"UNIMPLEMENTED opc_59 XO {xo} @ 0x{ins.Address:X8}")
                    };
                }

            case var s when s.StartsWith("fp_"):
                {
                    uint raw1 = ReadRawInstruction(ins);
                    // Extract real XO from bits 26-30
                    int xo = (int)((raw1 >> 1) & 0x1F);
                    
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rB = (int)((raw1 >> 11) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);

                    return xo switch
                    {
                        18 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), "fdiv") },
                        20 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), "sub") },
                        21 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), "add") },
                        22 => new[] { new IrCall($"f{rD}", "PPC_Fsqrt", new[] { IrValue.Register($"f{rB}") }) }, // fsqrt
                        23 => new[] { new IrCall($"f{rD}", "PPC_Fsel", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), IrValue.Register($"f{rC}") }) }, // fsel
                        24 => new[] { new IrCall($"f{rD}", "PPC_Fres", new[] { IrValue.Register($"f{rB}") }) }, // fres
                        25 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), "mul") }, // fmul uses A and C
                        26 => new[] { new IrCall($"f{rD}", "PPC_Frsqrte", new[] { IrValue.Register($"f{rB}") }) }, // frsqrte
                        28 => new[] { new IrCall($"f{rD}", "PPC_Fmsub", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fmsub
                        29 => new[] { new IrCall($"f{rD}", "PPC_Fmadd", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fmadd
                        30 => new[] { new IrCall($"f{rD}", "PPC_Fnmsub", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fnmsub
                        31 => new[] { new IrCall($"f{rD}", "PPC_Fnmadd", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // fnmadd
                        32 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rB}"), IrValue.Imm(0), "fneg") }, // fneg (unary)
                        12 => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rB}"), IrValue.Imm(0), "frsp") }, // frsp (unary)
                        8  => new[] { new IrBinary($"f{rD}", IrValue.Register($"f{rB}"), IrValue.Imm(0), "fcmp") }, // fcmp
                        _ => throw new NotImplementedException($"UNIMPLEMENTED fp_ fallback with XO {xo} @ 0x{ins.Address:X8}")
                    };
                }
        }

        return null;
    }
}
