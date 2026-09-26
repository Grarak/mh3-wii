using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class PpcDecoderAdditionalTests
{
    private static PpcInstruction Decode(uint word, uint address = 0x80000000u) => PpcDecoder.Decode(address, word);

    private static uint EncodeDForm(uint primary, uint rsOrRd, uint ra, int imm)
        => (primary << 26) | (rsOrRd << 21) | (ra << 16) | (uint)(ushort)imm;

    private static uint EncodeBc(uint bo, uint bi, int branchDelta, bool aa = false, bool lk = false)
        => (16u << 26) | (bo << 21) | (bi << 16) | ((((uint)branchDelta >> 2) & 0x3FFFu) << 2) | ((aa ? 1u : 0u) << 1) | (lk ? 1u : 0u);

    private static uint EncodeB(int branchDelta, bool aa = false, bool lk = false)
        => (18u << 26) | ((((uint)branchDelta >> 2) & 0x00FFFFFFu) << 2) | ((aa ? 1u : 0u) << 1) | (lk ? 1u : 0u);

    private static uint EncodeRotate(uint primary, uint rs, uint ra, uint shOrRb, uint mb, uint me, bool rc = false)
        => (primary << 26) | (rs << 21) | (ra << 16) | (shOrRb << 11) | (mb << 6) | (me << 1) | (rc ? 1u : 0u);

    private static uint EncodePs(uint frt, uint fra, uint frb, uint frc, uint xo, bool rc = false)
        => (4u << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | (rc ? 1u : 0u);

    private static uint EncodePsCmp(uint xo, uint crfD, uint fra, uint frb)
        => (4u << 26) | (crfD << 23) | (fra << 16) | (frb << 11) | (xo << 1);

    private static uint EncodePrimary63(uint frt, uint fra, uint frb, uint frc, uint xo, bool rc = false)
        => (63u << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | (rc ? 1u : 0u);

    private static uint EncodePrimary59(uint frt, uint fra, uint frb, uint frc, uint xo, bool rc = false)
        => (59u << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | (rc ? 1u : 0u);

    [Fact]
    public void DecodesImmediateBranchAndRotatePrimaries()
    {
        var twi = Decode(EncodeDForm(3, 31, 4, -2));
        Assert.Equal("twi", twi.Mnemonic);
        Assert.Collection(
            twi.Operands,
            op => Assert.Equal(31, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal("r4", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(-2, Assert.IsType<PpcImmediateOperand>(op).Value));

        var mulli = Decode(EncodeDForm(7, 3, 4, -2));
        Assert.Equal("mulli", mulli.Mnemonic);
        Assert.Collection(
            mulli.Operands,
            op => Assert.Equal("r3", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r4", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(-2, Assert.IsType<PpcImmediateOperand>(op).Value));

        var subfic = Decode(EncodeDForm(8, 5, 6, 0x1234));
        Assert.Equal("subfic", subfic.Mnemonic);

        var cmplwiCr0 = Decode(EncodeDForm(10, 0, 7, 0x55AA));
        Assert.Equal("cmplwi", cmplwiCr0.Mnemonic);
        Assert.Collection(
            cmplwiCr0.Operands,
            op => Assert.Equal("r7", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(0x55AA, Assert.IsType<PpcImmediateOperand>(op).Value));

        var addic = Decode(EncodeDForm(12, 8, 9, -4));
        Assert.Equal("addic", addic.Mnemonic);
        Assert.False(addic.IsConditionalBranch);

        var addicDot = Decode(EncodeDForm(13, 8, 9, -4));
        Assert.Equal("addic.", addicDot.Mnemonic);
        Assert.True(addicDot.IsConditionalBranch);

        Assert.Equal("li", Decode(EncodeDForm(14, 10, 0, -1)).Mnemonic);
        Assert.Equal("addi", Decode(EncodeDForm(14, 10, 11, 4)).Mnemonic);
        Assert.Equal("lis", Decode(EncodeDForm(15, 12, 0, 0x1234)).Mnemonic);
        Assert.Equal("addis", Decode(EncodeDForm(15, 12, 13, 0x1234)).Mnemonic);

        var genericBc = Decode(EncodeBc(0, 5, 0x20, aa: true, lk: true), address: 0x81234560);
        Assert.Equal("bcl", genericBc.Mnemonic);
        Assert.True(genericBc.IsCall);
        Assert.True(genericBc.IsConditionalBranch);
        Assert.Collection(
            genericBc.Operands,
            op => Assert.Equal(0, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal(5, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal("cr1", Assert.IsType<PpcConditionRegisterOperand>(op).Name),
            op => Assert.Equal(0x20u, Assert.IsType<PpcBranchTargetOperand>(op).TargetAddress));

        Assert.Equal("bdnz", Decode(EncodeBc(16, 0, 0x10)).Mnemonic);
        Assert.Equal("bdzl", Decode(EncodeBc(18, 0, 0x10, lk: true)).Mnemonic);

        var absoluteBl = Decode(EncodeB(0x40, aa: true, lk: true), address: 0x80001000);
        Assert.Equal("bl", absoluteBl.Mnemonic);
        Assert.True(absoluteBl.IsCall);
        Assert.Equal(0x40u, Assert.IsType<PpcBranchTargetOperand>(Assert.Single(absoluteBl.Operands)).TargetAddress);

        var sc = Decode(17u << 26);
        Assert.Equal("sc", sc.Mnemonic);
        Assert.Empty(sc.Operands);

        Assert.Equal("rlwimi.", Decode(EncodeRotate(20, 3, 4, 5, 6, 7, rc: true)).Mnemonic);
        Assert.Equal("rlwinm", Decode(EncodeRotate(21, 8, 9, 10, 11, 12)).Mnemonic);
        Assert.Equal("rlwnm.", Decode(EncodeRotate(23, 13, 14, 15, 16, 17, rc: true)).Mnemonic);
    }

    [Fact]
    public void DecodesPrimaryFourPairedSingleVariants()
    {
        Assert.Equal("ps_div.", Decode(EncodePs(1, 2, 3, 0, 18, rc: true)).Mnemonic);
        Assert.Equal("ps_sub", Decode(EncodePs(5, 6, 7, 0, 20)).Mnemonic);
        Assert.Equal("ps_add.", Decode(EncodePs(9, 10, 11, 0, 21, rc: true)).Mnemonic);
        Assert.Equal("ps_res", Decode(EncodePs(13, 14, 15, 0, 24)).Mnemonic);
        Assert.Equal("ps_rsqrte.", Decode(EncodePs(17, 18, 19, 0, 26, rc: true)).Mnemonic);

        Assert.Equal("ps_merge00", Decode(EncodePs(1, 2, 3, 0, 528)).Mnemonic);
        Assert.Equal("ps_merge01.", Decode(EncodePs(4, 5, 6, 0, 560, rc: true)).Mnemonic);
        Assert.Equal("ps_merge10", Decode(EncodePs(7, 8, 9, 0, 592)).Mnemonic);
        Assert.Equal("ps_merge11.", Decode(EncodePs(10, 11, 12, 0, 624, rc: true)).Mnemonic);

        Assert.Equal("ps_mr", Decode(EncodePs(13, 0, 14, 0, 72)).Mnemonic);
        Assert.Equal("ps_neg.", Decode(EncodePs(15, 0, 16, 0, 40, rc: true)).Mnemonic);
        Assert.Equal("ps_abs", Decode(EncodePs(17, 0, 18, 0, 264)).Mnemonic);
        Assert.Equal("ps_nabs.", Decode(EncodePs(19, 0, 20, 0, 136, rc: true)).Mnemonic);

        var cmpu0 = Decode(EncodePsCmp(0, 3, 4, 5));
        Assert.Equal("ps_cmpu0", cmpu0.Mnemonic);
        Assert.True(cmpu0.IsConditionalBranch);
        Assert.Equal("cr3", Assert.IsType<PpcConditionRegisterOperand>(cmpu0.Operands[0]).Name);

        Assert.Equal("ps_cmpo0", Decode(EncodePsCmp(32, 2, 6, 7)).Mnemonic);
        Assert.Equal("ps_cmpu1", Decode(EncodePsCmp(64, 1, 8, 9)).Mnemonic);
        Assert.Equal("ps_cmpo1", Decode(EncodePsCmp(96, 7, 10, 11)).Mnemonic);

        var dcbzL = Decode(EncodePs(0, 12, 13, 0, 1014));
        Assert.Equal("dcbz_l", dcbzL.Mnemonic);
        Assert.Collection(
            dcbzL.Operands,
            op => Assert.Equal("r12", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r13", Assert.IsType<PpcRegisterOperand>(op).Name));

        var psqLx = Decode(EncodePs(1, 2, 3, 0, 6) | (1u << 10) | (4u << 7));
        Assert.Equal("psq_lx", psqLx.Mnemonic);
        Assert.Collection(
            psqLx.Operands,
            op => Assert.Equal("f1", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r2", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r3", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(1, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal(4, Assert.IsType<PpcImmediateOperand>(op).Value));

        Assert.Equal("psq_stx", Decode(EncodePs(4, 5, 6, 0, 7)).Mnemonic);
        Assert.Equal("psq_lux", Decode(EncodePs(7, 8, 9, 0, 38)).Mnemonic);
        Assert.Equal("psq_stux", Decode(EncodePs(10, 11, 12, 0, 39)).Mnemonic);

        Assert.Equal("opc_4_17", Decode(EncodePs(1, 2, 3, 0, 17)).Mnemonic);
    }

    [Fact]
    public void DecodesLogicalDFormAndPsqMemoryPrimaries()
    {
        var nop = Decode(24u << 26);
        Assert.Equal("nop", nop.Mnemonic);
        Assert.Collection(
            nop.Operands,
            op => Assert.Equal("r0", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r0", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(0, Assert.IsType<PpcImmediateOperand>(op).Value));

        Assert.Equal("oris", Decode(EncodeDForm(25, 3, 4, 0x22)).Mnemonic);
        Assert.Equal("xori", Decode(EncodeDForm(26, 5, 6, 0x33)).Mnemonic);
        Assert.Equal("xoris", Decode(EncodeDForm(27, 7, 8, 0x44)).Mnemonic);

        var andi = Decode(EncodeDForm(28, 9, 10, 0x55));
        Assert.Equal("andi.", andi.Mnemonic);
        Assert.True(andi.IsConditionalBranch);

        var andis = Decode(EncodeDForm(29, 11, 12, 0x1234));
        Assert.Equal("andis.", andis.Mnemonic);
        Assert.True(andis.IsConditionalBranch);
        Assert.Collection(
            andis.Operands,
            op => Assert.Equal("r12", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r11", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal(0x1234, Assert.IsType<PpcImmediateOperand>(op).Value));

        Assert.Equal("lhz", Decode(EncodeDForm(40, 11, 12, -8)).Mnemonic);
        Assert.Equal("lhzu", Decode(EncodeDForm(41, 13, 14, 4)).Mnemonic);
        Assert.Equal("lha", Decode(EncodeDForm(42, 15, 16, 6)).Mnemonic);
        Assert.Equal("lhau", Decode(EncodeDForm(43, 16, 17, -6)).Mnemonic);
        Assert.Equal("sth", Decode(EncodeDForm(44, 17, 18, 8)).Mnemonic);
        Assert.Equal("sthu", Decode(EncodeDForm(45, 18, 19, -10)).Mnemonic);
        Assert.Equal("lmw", Decode(EncodeDForm(46, 19, 20, 12)).Mnemonic);
        Assert.Equal("stmw", Decode(EncodeDForm(47, 21, 22, 16)).Mnemonic);
        Assert.Equal("lfs", Decode(EncodeDForm(48, 1, 23, 20)).Mnemonic);
        Assert.Equal("lfsu", Decode(EncodeDForm(49, 2, 24, 24)).Mnemonic);
        Assert.Equal("lfd", Decode(EncodeDForm(50, 3, 25, 28)).Mnemonic);
        Assert.Equal("lfdu", Decode(EncodeDForm(51, 4, 26, 32)).Mnemonic);
        Assert.Equal("stfs", Decode(EncodeDForm(52, 5, 27, 36)).Mnemonic);
        Assert.Equal("stfsu", Decode(EncodeDForm(53, 6, 28, 40)).Mnemonic);
        Assert.Equal("stfd", Decode(EncodeDForm(54, 7, 29, 44)).Mnemonic);
        Assert.Equal("stfdu", Decode(EncodeDForm(55, 8, 30, 48)).Mnemonic);

        var psqLu = Decode(EncodeDForm(57, 9, 31, 0x5AB) | (1u << 15) | (3u << 12));
        Assert.Equal("psq_lu", psqLu.Mnemonic);
        Assert.Collection(
            psqLu.Operands,
            op => Assert.Equal("f9", Assert.IsType<PpcRegisterOperand>(op).Name),
            op =>
            {
                var disp = Assert.IsType<PpcDisplacementOperand>(op);
                Assert.Equal(0x5AB, disp.Offset);
                Assert.Equal("r31", disp.BaseRegister);
            },
            op => Assert.Equal(1, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal(3, Assert.IsType<PpcImmediateOperand>(op).Value));

        var psqStu = Decode(EncodeDForm(61, 10, 30, -1) | (1u << 15) | (7u << 12));
        Assert.Equal("psq_stu", psqStu.Mnemonic);
        Assert.Equal(-1, Assert.IsType<PpcDisplacementOperand>(psqStu.Operands[1]).Offset);
    }

    [Fact]
    public void DecodesPrimary63FloatingFormsAndFallback()
    {
        var fcmpu = Decode(EncodePrimary63(0, 2, 3, 0, 0));
        Assert.Equal("fcmpu", fcmpu.Mnemonic);
        Assert.True(fcmpu.IsConditionalBranch);

        Assert.Equal("fcmpo", Decode(EncodePrimary63(0, 4, 5, 0, 32)).Mnemonic);
        Assert.Equal("fctiw.", Decode(EncodePrimary63(6, 0, 7, 0, 14, rc: true)).Mnemonic);
        Assert.Equal("fctiwz.", Decode(EncodePrimary63(6, 0, 7, 0, 15, rc: true)).Mnemonic);
        Assert.Equal("fdiv", Decode(EncodePrimary63(8, 9, 10, 0, 18)).Mnemonic);
        Assert.Equal("fsub.", Decode(EncodePrimary63(11, 12, 13, 0, 20, rc: true)).Mnemonic);
        Assert.Equal("fadd", Decode(EncodePrimary63(14, 15, 16, 0, 21)).Mnemonic);
        Assert.Equal("frsp", Decode(EncodePrimary63(14, 0, 16, 0, 12)).Mnemonic);
        Assert.Equal("fsel.", Decode(EncodePrimary63(14, 15, 16, 17, 23, rc: true)).Mnemonic);
        Assert.Equal("fres", Decode(EncodePrimary63(14, 0, 16, 0, 24)).Mnemonic);
        Assert.Equal("fmul.", Decode(EncodePrimary63(17, 18, 19, 0, 25, rc: true)).Mnemonic);
        Assert.Equal("frsqrte", Decode(EncodePrimary63(17, 0, 19, 0, 26)).Mnemonic);
        Assert.Equal("fmsub", Decode(EncodePrimary63(17, 18, 19, 20, 28)).Mnemonic);
        Assert.Equal("fmadd", Decode(EncodePrimary63(17, 18, 19, 20, 29)).Mnemonic);
        Assert.Equal("fnmsub", Decode(EncodePrimary63(17, 18, 19, 20, 30)).Mnemonic);
        Assert.Equal("fnmadd", Decode(EncodePrimary63(17, 18, 19, 20, 31)).Mnemonic);
        Assert.Equal("mtfsb1", Decode(EncodePrimary63(17, 0, 0, 0, 38)).Mnemonic);
        Assert.Equal("mtfsb0", Decode(EncodePrimary63(18, 0, 0, 0, 70)).Mnemonic);
        var mtfsfi = Decode((63u << 26) | (3u << 23) | (0xCu << 12) | (134u << 1));
        Assert.Equal("mtfsfi", mtfsfi.Mnemonic);
        Assert.Collection(
            mtfsfi.Operands,
            op => Assert.Equal(3, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal(0xC, Assert.IsType<PpcImmediateOperand>(op).Value));
        Assert.Equal("fneg", Decode(EncodePrimary63(21, 0, 22, 0, 40)).Mnemonic);
        Assert.Equal("fmr.", Decode(EncodePrimary63(23, 0, 24, 0, 72, rc: true)).Mnemonic);
        Assert.Equal("fnabs", Decode(EncodePrimary63(25, 0, 26, 0, 136)).Mnemonic);
        Assert.Equal("fabs", Decode(EncodePrimary63(25, 0, 26, 0, 264)).Mnemonic);
        Assert.Equal("mffs", Decode(EncodePrimary63(27, 0, 0, 0, 583)).Mnemonic);
        var mtfsf = Decode(EncodePrimary63(0, 0, 7, 0, 711) | (0xAAu << 17));
        Assert.Equal("mtfsf", mtfsf.Mnemonic);
        Assert.Collection(
            mtfsf.Operands,
            op => Assert.Equal(0xAA, Assert.IsType<PpcImmediateOperand>(op).Value),
            op => Assert.Equal("f7", Assert.IsType<PpcRegisterOperand>(op).Name));
        Assert.Equal("fp_27", Decode(EncodePrimary63(1, 2, 3, 0, 27)).Mnemonic);
    }

    [Fact]
    public void DecodesPrimaryFiftyNineSinglePrecisionForms()
    {
        Assert.Equal("fdivs", Decode(EncodePrimary59(1, 2, 3, 4, 18)).Mnemonic);
        Assert.Equal("fsubs.", Decode(EncodePrimary59(1, 2, 3, 4, 20, rc: true)).Mnemonic);
        Assert.Equal("fadds", Decode(EncodePrimary59(1, 2, 3, 4, 21)).Mnemonic);
        Assert.Equal("fres", Decode(EncodePrimary59(1, 2, 3, 4, 24)).Mnemonic);
        Assert.Equal("fmuls.", Decode(EncodePrimary59(1, 2, 3, 4, 25, rc: true)).Mnemonic);
        Assert.Equal("fmsubs", Decode(EncodePrimary59(1, 2, 3, 4, 28)).Mnemonic);
        Assert.Equal("fmadds", Decode(EncodePrimary59(1, 2, 3, 4, 29)).Mnemonic);
        Assert.Equal("fnmsubs", Decode(EncodePrimary59(1, 2, 3, 4, 30)).Mnemonic);
        Assert.Equal("fnmadds", Decode(EncodePrimary59(1, 2, 3, 4, 31)).Mnemonic);
        Assert.Equal("opc_59", Decode(EncodePrimary59(1, 2, 3, 4, 27)).Mnemonic);
    }
}
