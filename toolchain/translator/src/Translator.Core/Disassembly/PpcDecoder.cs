
namespace Translator.Core.Disassembly;

/// <summary>
/// Minimal in-house PowerPC decoder for the Wii/Gekko instruction set. It returns typed
/// operands so later stages never have to parse disassembly strings.
/// </summary>
public static partial class PpcDecoder
{
    public static PpcInstruction Decode(uint address, uint word)
    {
        var primary = (int)((word >> 26) & 0x3F);
        var operands = new List<PpcOperand>();
        var branches = new List<uint>();
        var mnemonic = "unknown";
        var isReturn = false;
        var isCall = false;
        var isCond = false;

        int rD, rA, rS;

        switch (primary)
        {
            case 3: // twi
                rA = GetField(word, 16);
                mnemonic = "twi";
                operands.Add(new PpcImmediateOperand(GetField(word, 21, 5)));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                break;

            case 7: // mulli
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "mulli";
                operands.Add(Reg(rD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                break;

            case 8: // subfic
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "subfic";
                operands.Add(Reg(rD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                break;

            case 10: // cmplwi
            {
                var crfD = GetField(word, 23, 3); // Bits 6-8 (3 bits)
                rA = GetField(word, 16);
                mnemonic = "cmplwi";
                if (crfD != 0)
                    operands.Add(CrField(crfD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((int)(word & 0xFFFF)));
                isCond = true;
                break;
            }

            case 11: // cmpwi
            {
                var crfD = GetField(word, 23, 3); // Bits 6-8 (3 bits)
                rA = GetField(word, 16);
                mnemonic = "cmpwi";
                if (crfD != 0)
                    operands.Add(CrField(crfD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                isCond = true;
                break;
            }

            case 12: // addic
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "addic";
                operands.Add(Reg(rD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                break;

            case 13: // addic.
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "addic.";
                operands.Add(Reg(rD));
                operands.Add(Reg(rA));
                operands.Add(new PpcImmediateOperand((short)(word & 0xFFFF)));
                isCond = true;
                break;

            case 14: // addi / li
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                var simm = (short)(word & 0xFFFF);
                if (rA == 0)
                {
                    mnemonic = "li";
                    operands.Add(Reg(rD));
                    operands.Add(new PpcImmediateOperand(simm));
                }
                else
                {
                    mnemonic = "addi";
                    operands.Add(Reg(rD));
                    operands.Add(Reg(rA));
                    operands.Add(new PpcImmediateOperand(simm));
                }
                break;

            case 15: // addis / lis
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                simm = (short)(word & 0xFFFF);
                if (rA == 0)
                {
                    mnemonic = "lis";
                    operands.Add(Reg(rD));
                    operands.Add(new PpcImmediateOperand(simm));
                }
                else
                {
                    mnemonic = "addis";
                    operands.Add(Reg(rD));
                    operands.Add(Reg(rA));
                    operands.Add(new PpcImmediateOperand(simm));
                }
                break;

            case 16: // bc and its extended mnemonics
                var bo = GetField(word, 21, 5);
                var bi = GetField(word, 16, 5);
                var bd = SignExtend((word >> 2) & 0x3FFFu, 14) << 2;
                var aa = ((word >> 1) & 1) != 0;
                var lk = (word & 1) != 0;  // LK bit - set link register
                var target = aa ? (uint)bd : address + (uint)bd;
                var crField = bi / 4;  // CR0-CR7 field

                var branchMnemonic = TryDecodeCrBranchMnemonic(bo, bi);
                if (branchMnemonic is not null)
                {
                    mnemonic = lk ? branchMnemonic + "l" : branchMnemonic;
                }
                else if ((bo & 0x1E) == 16)
                {
                    mnemonic = lk ? "bdnzl" : "bdnz";
                }
                else if ((bo & 0x1E) == 18)
                {
                    mnemonic = lk ? "bdzl" : "bdz";
                }
                else
                {
                    mnemonic = lk ? "bcl" : "bc";
                    operands.Add(new PpcImmediateOperand(bo));
                    operands.Add(new PpcImmediateOperand(bi));
                }

                // Add CR field operand for non-cr0 branches so the lifter knows which field to check
                if (crField != 0)
                {
                    operands.Add(CrField(crField));
                }
                operands.Add(new PpcBranchTargetOperand(target));
                branches.Add(target);
                isCond = true;
                isCall = lk;  // LK bit means this is a call (sets LR)
                break;

            case 17: // sc
                mnemonic = "sc";
                break;


            case 18: // b / bl
                var li = SignExtend((word >> 2) & 0x00FFFFFFu, 24) << 2;
                aa = ((word >> 1) & 1) != 0;
                lk = (word & 1) != 0;
                target = aa ? (uint)li : address + (uint)li;
                mnemonic = lk ? "bl" : "b";
                operands.Add(new PpcBranchTargetOperand(target));
                branches.Add(target);
                isCall = lk;
                break;

            case 19:
                DecodePrimary19(address, word, operands, branches, ref mnemonic, ref isReturn, ref isCall, ref isCond);
                break;
            case 20: // rlwimi
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                var sh = GetField(word, 11, 5);
                var mb = GetField(word, 6, 5);
                var me = GetField(word, 1, 5);
                mnemonic = (word & 1) != 0 ? "rlwimi." : "rlwimi";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(sh));
                operands.Add(new PpcImmediateOperand(mb));
                operands.Add(new PpcImmediateOperand(me));
                isCond = (word & 1) != 0;
                break;

            case 21: // rlwinm
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                sh = GetField(word, 11, 5);
                mb = GetField(word, 6, 5);
                me = GetField(word, 1, 5);
                mnemonic = (word & 1) != 0 ? "rlwinm." : "rlwinm";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(sh));
                operands.Add(new PpcImmediateOperand(mb));
                operands.Add(new PpcImmediateOperand(me));
                isCond = (word & 1) != 0;
                break;

            case 23: // rlwnm - Rotate Left Word then AND with Mask (register shift)
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                var rB = GetField(word, 11);  // Register containing shift amount
                mb = GetField(word, 6, 5);
                me = GetField(word, 1, 5);
                mnemonic = (word & 1) != 0 ? "rlwnm." : "rlwnm";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(Reg(rB));  // Register instead of immediate
                operands.Add(new PpcImmediateOperand(mb));
                operands.Add(new PpcImmediateOperand(me));
                isCond = (word & 1) != 0;
                break;

            case 4: // paired-single arithmetic/select
            {
                // Extract full 10-bit XO (bits 21-30)
                var xo = (int)((word >> 1) & 0x3FF);
                // Extract lower 5 bits (bits 26-30)
                var xo5 = xo & 0x1F;
                var rc = (word & 1) != 0;

                var frt = GetField(word, 21);
                var fra = GetField(word, 16);
                var frb = GetField(word, 11);
                var frc = GetField(word, 6);

                // First handle A-form instructions (which use bits 21-25 as frC operand, not part of opcode)
                // These are identified by the lower 5 bits of XO (bits 26-30 in word)
                switch (xo5)
                {
                    case 29: // ps_madd (XO=29)
                        mnemonic = rc ? "ps_madd." : "ps_madd";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 28: // ps_msub (XO=28)
                        mnemonic = rc ? "ps_msub." : "ps_msub";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 30: // ps_nmsub (XO=30)
                        mnemonic = rc ? "ps_nmsub." : "ps_nmsub";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 31: // ps_nmadd (XO=31)
                        mnemonic = rc ? "ps_nmadd." : "ps_nmadd";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 25: // ps_mul (XO=25)
                        mnemonic = rc ? "ps_mul." : "ps_mul";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        break;

                    case 23: // ps_sel (XO=23)
                        mnemonic = rc ? "ps_sel." : "ps_sel";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 14: // ps_madds0 (XO5=14)
                        mnemonic = rc ? "ps_madds0." : "ps_madds0";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 15: // ps_madds1 (XO5=15)
                        mnemonic = rc ? "ps_madds1." : "ps_madds1";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 10: // ps_sum0 (XO5=10)
                        mnemonic = rc ? "ps_sum0." : "ps_sum0";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 11: // ps_sum1 (XO5=11)
                        mnemonic = rc ? "ps_sum1." : "ps_sum1";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        operands.Add(FReg(frb));
                        break;

                    case 12: // ps_muls0 (XO5=12)
                        mnemonic = rc ? "ps_muls0." : "ps_muls0";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        break;

                    case 13: // ps_muls1 (XO5=13)
                        mnemonic = rc ? "ps_muls1." : "ps_muls1";
                        operands.Add(FReg(frt));
                        operands.Add(FReg(fra));
                        operands.Add(FReg(frc));
                        break;

                    default:
                        // If not A-form, check full 10-bit XO for X-form instructions
                        var psqIndexedXo = xo & 0x3F;
                        if (psqIndexedXo is 6 or 38)
                        {
                            var w = (word >> 10) & 0x1;
                            var quant = (word >> 7) & 0x7;
                            mnemonic = psqIndexedXo == 38 ? "psq_lux" : "psq_lx";
                            operands.Add(FReg(frt));
                            operands.Add(Reg(fra));
                            operands.Add(Reg(frb));
                            operands.Add(new PpcImmediateOperand((int)w));
                            operands.Add(new PpcImmediateOperand((int)quant));
                            break;
                        }

                        if (psqIndexedXo is 7 or 39)
                        {
                            var w = (word >> 10) & 0x1;
                            var quant = (word >> 7) & 0x7;
                            mnemonic = psqIndexedXo == 39 ? "psq_stux" : "psq_stx";
                            operands.Add(FReg(frt));
                            operands.Add(Reg(fra));
                            operands.Add(Reg(frb));
                            operands.Add(new PpcImmediateOperand((int)w));
                            operands.Add(new PpcImmediateOperand((int)quant));
                            break;
                        }

                        switch (xo)
                        {
                            case 21: // ps_add (XO=21)
                                mnemonic = rc ? "ps_add." : "ps_add";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 20: // ps_sub (XO=20)
                                mnemonic = rc ? "ps_sub." : "ps_sub";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 18: // ps_div (XO=18)
                                mnemonic = rc ? "ps_div." : "ps_div";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 24: // ps_res (XO=24)
                                mnemonic = rc ? "ps_res." : "ps_res";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 26: // ps_rsqrte (XO=26)
                                mnemonic = rc ? "ps_rsqrte." : "ps_rsqrte";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 528: // ps_merge00 (XO=528)
                                mnemonic = rc ? "ps_merge00." : "ps_merge00";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 560: // ps_merge01 (XO=560)
                                mnemonic = rc ? "ps_merge01." : "ps_merge01";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 592: // ps_merge10 (XO=592)
                                mnemonic = rc ? "ps_merge10." : "ps_merge10";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 624: // ps_merge11 (XO=624)
                                mnemonic = rc ? "ps_merge11." : "ps_merge11";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                break;

                            case 72: // ps_mr (XO=72)
                                mnemonic = rc ? "ps_mr." : "ps_mr";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 40: // ps_neg (XO=40)
                                mnemonic = rc ? "ps_neg." : "ps_neg";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 264: // ps_abs (XO=264)
                                mnemonic = rc ? "ps_abs." : "ps_abs";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 136: // ps_nabs (XO=136)
                                mnemonic = rc ? "ps_nabs." : "ps_nabs";
                                operands.Add(FReg(frt));
                                operands.Add(FReg(frb));
                                break;

                            case 0: // ps_cmpu0 (XO=0)
                            {
                                var crfD = GetField(word, 23, 3);
                                mnemonic = "ps_cmpu0";
                                operands.Add(CrField(crfD));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                isCond = true;
                                break;
                            }

                            case 32: // ps_cmpo0 (XO=32)
                            {
                                var crfD = GetField(word, 23, 3);
                                mnemonic = "ps_cmpo0";
                                operands.Add(CrField(crfD));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                isCond = true;
                                break;
                            }

                            case 64: // ps_cmpu1 (XO=64)
                            {
                                var crfD = GetField(word, 23, 3);
                                mnemonic = "ps_cmpu1";
                                operands.Add(CrField(crfD));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                isCond = true;
                                break;
                            }

                            case 96: // ps_cmpo1 (XO=96)
                            {
                                var crfD = GetField(word, 23, 3);
                                mnemonic = "ps_cmpo1";
                                operands.Add(CrField(crfD));
                                operands.Add(FReg(fra));
                                operands.Add(FReg(frb));
                                isCond = true;
                                break;
                            }

                            case 1014: // dcbz_l (XO=1014)
                                mnemonic = "dcbz_l";
                                operands.Add(Reg(fra));
                                operands.Add(Reg(frb));
                                break;

                            default:
                                mnemonic = $"opc_4_{xo}";
                                break;
                        }
                        break;
                }
                break;
            }

            case 24: // ori / nop alias
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                var uimm16 = (int)(word & 0xFFFF);
                mnemonic = (rA == 0 && rS == 0 && uimm16 == 0) ? "nop" : "ori";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                break;

            case 25: // oris
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                uimm16 = (int)(word & 0xFFFF);
                mnemonic = "oris";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                break;

            case 26: // xori
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                uimm16 = (int)(word & 0xFFFF);
                mnemonic = "xori";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                break;

            case 27: // xoris
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                uimm16 = (int)(word & 0xFFFF);
                mnemonic = "xoris";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                break;

            case 28: // andi.
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                uimm16 = (int)(word & 0xFFFF);
                mnemonic = "andi.";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                isCond = true;
                break;

            case 29: // andis.
                rA = GetField(word, 16);
                rS = GetField(word, 21);
                uimm16 = (int)(word & 0xFFFF);
                mnemonic = "andis.";
                operands.Add(Reg(rA));
                operands.Add(Reg(rS));
                operands.Add(new PpcImmediateOperand(uimm16));
                isCond = true;
                break;

            case 32: // lwz
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lwz";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 33: // lwzu
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lwzu";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 34: // lbz
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lbz";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 35: // lbzu
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lbzu";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 36: // stw
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stw";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 37: // stwu
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stwu";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 38: // stb
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stb";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 39: // stbu
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stbu";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 40: // lhz
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lhz";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 41: // lhzu
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lhzu";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 42: // lha
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lha";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 43: // lhau
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lhau";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 44: // sth
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "sth";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 45: // sthu
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "sthu";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 46: // lmw
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lmw";
                operands.Add(Reg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 47: // stmw
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stmw";
                operands.Add(Reg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 48: // lfs
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lfs";
                operands.Add(FReg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 49: // lfsu
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lfsu";
                operands.Add(FReg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 50: // lfd
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lfd";
                operands.Add(FReg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 51: // lfdu
                rD = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "lfdu";
                operands.Add(FReg(rD));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 52: // stfs
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stfs";
                operands.Add(FReg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 53: // stfsu
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stfsu";
                operands.Add(FReg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 54: // stfd
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stfd";
                operands.Add(FReg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 55: // stfdu
                rS = GetField(word, 21);
                rA = GetField(word, 16);
                mnemonic = "stfdu";
                operands.Add(FReg(rS));
                operands.Add(new PpcDisplacementOperand((short)(word & 0xFFFF), RegName(rA), rA));
                break;

            case 56: // psq_l
            case 57: // psq_lu
            case 60: // psq_st
            case 61: // psq_stu
                {
                    var fr = GetField(word, 21);
                    rA = GetField(word, 16);
                    var w = (word >> 15) & 0x1;
                    var quant = (word >> 12) & 0x7;
                    var disp = SignExtend(word & 0xFFFu, 12);

                    mnemonic = primary switch
                    {
                        56 => "psq_l",
                        57 => "psq_lu",
                        60 => "psq_st",
                        61 => "psq_stu",
                        _ => $"opc_{primary}"
                    };

                    operands.Add(FReg(fr));
                    operands.Add(new PpcDisplacementOperand(disp, RegName(rA), rA));
                    operands.Add(new PpcImmediateOperand((int)w));
                    operands.Add(new PpcImmediateOperand((int)quant));
                    break;
                }

            case 63:
                DecodePrimary63(address, word, operands, branches, ref mnemonic, ref isReturn, ref isCall, ref isCond);
                break;
            case 59:
                DecodePrimary59(address, word, operands, branches, ref mnemonic, ref isReturn, ref isCall, ref isCond);
                break;
            case 31:
                DecodePrimary31(address, word, operands, branches, ref mnemonic, ref isReturn, ref isCall, ref isCond);
                break;
            default:
                mnemonic = $"opc_{primary}";
                break;
        }

        return new PpcInstruction(address, word, mnemonic, operands, branches, isReturn, isCall, isCond);
    }

}
