using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

public sealed partial class PpcLifter
{
    private static IReadOnlyList<IrInstruction>? TryLiftPairedSingle(
        PpcInstruction ins,
        string mnemonic,
        IReadOnlyList<PpcOperand> ops,
        IReadOnlyList<string> operands)
    {
        string Reg(int idx) => RegisterOperandName(ops, idx);
        int Imm(int idx) => ImmediateOperandValue(ops, idx);
        switch (mnemonic)
        {
            case "psq_l" when ops.Count == 4:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return EmitPairedSingleLoad(Reg(0), reg, offset, Imm(2), Imm(3), updateBase: false, ins.Address);
                }

            case "psq_lu" when ops.Count == 4:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return EmitPairedSingleLoad(Reg(0), reg, offset, Imm(2), Imm(3), updateBase: true, ins.Address);
                }

            case "psq_st" when ops.Count == 4:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return EmitPairedSingleStore(Reg(0), reg, offset, Imm(2), Imm(3), updateBase: false, ins.Address);
                }

            case "psq_stu" when ops.Count == 4:
                {
                    var (offset, reg) = ParseDisplacement(operands[1]);
                    return EmitPairedSingleStore(Reg(0), reg, offset, Imm(2), Imm(3), updateBase: true, ins.Address);
                }

            case "psq_lx":
                return LiftPairedSingleIndexedLoad(ins, ReadRawInstruction(ins), updateBase: false);

            case "psq_lux":
                return LiftPairedSingleIndexedLoad(ins, ReadRawInstruction(ins), updateBase: true);

            case "psq_stx":
                return LiftPairedSingleIndexedStore(ins, ReadRawInstruction(ins), updateBase: false);

            case "psq_stux":
                return LiftPairedSingleIndexedStore(ins, ReadRawInstruction(ins), updateBase: true);

            case "ps_add" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsAdd", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_sub" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsSub", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_div" when ops.Count == 3:
            case "ps_div." when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsDiv", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_mul" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMul", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_msub" when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsMsub", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_madd" when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsMadd", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_nmsub" when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsNmsub", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_nmadd" when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsNmadd", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_madds0" when ops.Count == 4:
            case "ps_madds0." when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsMadds0", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_madds1" when ops.Count == 4:
            case "ps_madds1." when ops.Count == 4:
                return new[] { new IrCall(Reg(0), "PPC_PsMadds1", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)), IrValue.Register(Reg(3)) }) };

            case "ps_sum0" when ops.Count == 4:
            case "ps_sum0." when ops.Count == 4:
                // Decoder outputs (D, A, C, B) per IBM syntax, but PPC_PsSum0 expects (frA, frB, frC)
                // So we pass Reg(1)=frA, Reg(3)=frB, Reg(2)=frC
                return new[] { new IrCall(Reg(0), "PPC_PsSum0", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(3)), IrValue.Register(Reg(2)) }) };

            case "ps_sum1" when ops.Count == 4:
            case "ps_sum1." when ops.Count == 4:
                // Decoder outputs (D, A, C, B) per IBM syntax, but PPC_PsSum1 expects (frA, frB, frC)
                // So we pass Reg(1)=frA, Reg(3)=frB, Reg(2)=frC
                return new[] { new IrCall(Reg(0), "PPC_PsSum1", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(3)), IrValue.Register(Reg(2)) }) };

            case "ps_muls0" when ops.Count == 3:
            case "ps_muls0." when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMuls0", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_muls1" when ops.Count == 3:
            case "ps_muls1." when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMuls1", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_sel" when ops.Count == 4:
                {
                    // Check for misidentified ps_neg (XO 40 encoded in bits 21-30)
                    uint rawSel = ReadRawInstruction(ins);
                    int selXo = (int)((rawSel >> 1) & 0x3FF);
                    if (selXo == 40)
                    {
                        // ps_neg frD, frB (D is bits 6-10, B is bits 16-20)
                        int selRD = (int)((rawSel >> 21) & 0x1F);
                        int selRB = (int)((rawSel >> 11) & 0x1F);
                        return new[] { new IrCall($"f{selRD}", "PPC_PsNeg", new[] { IrValue.Register($"f{selRB}") }) };
                    }

                    // ps_sel frD, frA, frC, frB
                    // frD = frA >= 0 ? frC : frB
                    // PPC_PsSel(lhs=frC, control=frA, rhs=frB)
                    return new[] { new IrCall(Reg(0), "PPC_PsSel", new[] { IrValue.Register(Reg(2)), IrValue.Register(Reg(1)), IrValue.Register(Reg(3)) }) };
                }

            case "ps_res" when ops.Count == 2:
                return new[] { new IrCall(Reg(0), "PPC_PsRes", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_rsqrte" when ops.Count == 2:
                return new[] { new IrCall(Reg(0), "PPC_PsRsqrte", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_neg":
            case "ps_neg.":
                return new[] { new IrCall(Reg(0), "PPC_PsNeg", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_abs":
            case "ps_abs.":
                return new[] { new IrCall(Reg(0), "PPC_PsAbs", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_nabs":
            case "ps_nabs.":
                return new[] { new IrCall(Reg(0), "PPC_PsNabs", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_merge00" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMerge00", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_merge01" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMerge01", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_merge10" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMerge10", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_merge11" when ops.Count == 3:
                return new[] { new IrCall(Reg(0), "PPC_PsMerge11", new[] { IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };

            case "ps_mr" when ops.Count == 2:
            case "ps_mr." when ops.Count == 2:
                // ps_mr copies both lanes, unlike scalar fmr which writes only PS0
                // and preserves the destination's existing PS1 lane.
                return new[] { new IrCall(Reg(0), "PPC_PsMr", new[] { IrValue.Register(Reg(1)) }) };

            case "ps_cmpo0" when ops.Count == 3:
            case "ps_cmpo0." when ops.Count == 3: // Dot form doesn't really exist for compare but just in case
                {
                    var dest = operands[0]; // cr field
                    var crField = ParseCrFieldName(dest);
                    // PPC_PsCmpo0(crField, frA, frB)
                    return new[] { new IrCall(string.Empty, "PPC_PsCmpo0", new[] { IrValue.Imm(crField), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };
                }

            case "ps_cmpu0" when ops.Count == 3:
            case "ps_cmpu0." when ops.Count == 3:
                {
                    var dest = operands[0];
                    var crField = ParseCrFieldName(dest);
                    return new[] { new IrCall(string.Empty, "PPC_PsCmpu0", new[] { IrValue.Imm(crField), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };
                }

            case "ps_cmpo1" when ops.Count == 3:
            case "ps_cmpo1." when ops.Count == 3:
                {
                    var dest = operands[0];
                    var crField = ParseCrFieldName(dest);
                    return new[] { new IrCall(string.Empty, "PPC_PsCmpo1", new[] { IrValue.Imm(crField), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };
                }

            case "ps_cmpu1" when ops.Count == 3:
            case "ps_cmpu1." when ops.Count == 3:
                {
                    var dest = operands[0];
                    var crField = ParseCrFieldName(dest);
                    return new[] { new IrCall(string.Empty, "PPC_PsCmpu1", new[] { IrValue.Imm(crField), IrValue.Register(Reg(1)), IrValue.Register(Reg(2)) }) };
                }

            // Some toolchains emit raw opcode labels (opc_4_XX) for paired-single ops.
            // Map the ones we know we use back to the corresponding ps_* helpers.
            case "opc_4_50": // ps_mul (XO 25, Rc=0)
            case "opc_4_51": // ps_mul. (XO 25, Rc=1)
                {
                    uint raw1 = ReadRawInstruction(ins);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);
                    return new[] { new IrCall($"f{rD}", "PPC_PsMul", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}") }) };
                }

            case var s when s.StartsWith("opc_4_"):
                {
                    if (!int.TryParse(s.Split('_').Last(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var subop))
                    {
                        throw new NotImplementedException($"UNIMPLEMENTED paired-single opcode '{s}' @ 0x{ins.Address:X8}");
                    }

                    uint raw1 = ReadRawInstruction(ins);
                    int rD = (int)((raw1 >> 21) & 0x1F);
                    int rA = (int)((raw1 >> 16) & 0x1F);
                    int rB = (int)((raw1 >> 11) & 0x1F);
                    int rC = (int)((raw1 >> 6) & 0x1F);

                    // Some paired-single instructions use X-form encoding with 10-bit XO in bits 1-10.
                    // The disassembler may give us the wrong A-form subop, so check X-form XO first.
                    int xformXO = (int)((raw1 >> 1) & 0x3FF); // bits 1-10
                    if (xformXO == 40) // ps_neg: frD = -frB (paired)
                    {
                        return new[] { new IrCall($"f{rD}", "PPC_PsNeg", new[] { IrValue.Register($"f{rB}") }) };
                    }

                    return subop switch
                    {
                        0 or 1 or 2 or 3 => LiftCompare("cr0", IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), "sub", isUnsigned: false),
                        4 => new[] { new IrCall($"f{rD}", "PPC_PsMr", new[] { IrValue.Register($"f{rB}") }) },
                        5 => new[] { new IrCall($"f{rD}", "PPC_PsNabs", new[] { IrValue.Register($"f{rB}") }) },
                        6 or 12 => LiftPairedSingleIndexedLoad(ins, raw1, updateBase: false), // psq_lx (XO=6, 6<<1=12)
                        7 or 14 => LiftPairedSingleIndexedStore(ins, raw1, updateBase: false), // psq_stx (XO=7, 7<<1=14)
                        
                        // Validated Arithmetic Ops (XO << 1 | Rc)
                        20 or 21 => new[] { new IrCall($"f{rD}", "PPC_PsSum0", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), IrValue.Register($"f{rC}") }) }, // ps_sum0 (XO=10)
                        22 or 23 => new[] { new IrCall($"f{rD}", "PPC_PsSum1", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}"), IrValue.Register($"f{rC}") }) }, // ps_sum1 (XO=11)
                        24 or 25 => new[] { new IrCall($"f{rD}", "PPC_PsMuls0", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}") }) }, // ps_muls0 (XO=12)
                        26 or 27 => new[] { new IrCall($"f{rD}", "PPC_PsMuls1", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}") }) }, // ps_muls1 (XO=13)
                        28 or 29 => new[] { new IrCall($"f{rD}", "PPC_PsMadds0", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_madds0 (XO=14)
                        30 or 31 => new[] { new IrCall($"f{rD}", "PPC_PsMadds1", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_madds1 (XO=15)
                        
                        36 or 37 => new[] { new IrCall($"f{rD}", "PPC_PsDiv", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // ps_div (XO=18)
                        40 or 41 => new[] { new IrCall($"f{rD}", "PPC_PsSub", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // ps_sub (XO=20)
                        42 or 43 => new[] { new IrCall($"f{rD}", "PPC_PsAdd", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // ps_add (XO=21)
                        44 or 45 => new[] { new IrCall($"f{rD}", "PPC_PsAbs", new[] { IrValue.Register($"f{rB}") }) }, // ps_abs (XO=22)
                        46 or 47 => new[] { new IrCall($"f{rD}", "PPC_PsSel", new[] { IrValue.Register($"f{rC}"), IrValue.Register($"f{rA}"), IrValue.Register($"f{rB}") }) }, // ps_sel (XO=23)
                        48 or 49 => new[] { new IrCall($"f{rD}", "PPC_PsRes", new[] { IrValue.Register($"f{rB}") }) }, // ps_res (XO=24)
                        50 or 51 => new[] { new IrCall($"f{rD}", "PPC_PsMul", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}") }) }, // ps_mul (XO=25)
                        52 or 53 => new[] { new IrCall($"f{rD}", "PPC_PsRsqrte", new[] { IrValue.Register($"f{rB}") }) }, // ps_rsqrte (XO=26)
                        
                        56 or 57 => new[] { new IrCall($"f{rD}", "PPC_PsMsub", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_msub (XO=28)
                        58 or 59 => new[] { new IrCall($"f{rD}", "PPC_PsMadd", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_madd (XO=29)
                        60 or 61 => new[] { new IrCall($"f{rD}", "PPC_PsNmsub", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_nmsub (XO=30)
                        62 or 63 => new[] { new IrCall($"f{rD}", "PPC_PsNmadd", new[] { IrValue.Register($"f{rA}"), IrValue.Register($"f{rC}"), IrValue.Register($"f{rB}") }) }, // ps_nmadd (XO=31)
                        
                        // Memory Ops (using raw XO?)
                        38 or 76 => LiftPairedSingleIndexedLoad(ins, raw1, updateBase: true), // psq_lux (XO=38, 38<<1=76)
                        39 or 78 => LiftPairedSingleIndexedStore(ins, raw1, updateBase: true), // psq_stux (XO=39, 39<<1=78)
                        _ => throw new NotImplementedException($"UNIMPLEMENTED paired-single opcode '{s}' @ 0x{ins.Address:X8}")
                    };
                }
        }

        return null;
    }
}
